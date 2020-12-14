/*
 * Copyright (C) 2016 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/range/algorithm/heap_algorithm.hpp>
#include <seastar/util/defer.hh>

#include "partition_version.hh"
#include "row_cache.hh"
#include "partition_snapshot_row_cursor.hh"
#include "utils/coroutine.hh"
#include "real_dirty_memory_accounter.hh"

static void remove_or_mark_as_unique_owner(partition_version* current, mutation_cleaner* cleaner)
{
    while (current && !current->is_referenced()) {
        auto next = current->next();
        current->erase();
        if (cleaner) {
            cleaner->destroy_gently(*current);
        } else {
            current_allocator().destroy(current);
        }
        current = next;
    }
    if (current) {
        current->back_reference().mark_as_unique_owner();
    }
}

partition_version::partition_version(partition_version&& pv) noexcept
    : anchorless_list_base_hook(std::move(pv))
    , _backref(pv._backref)
    , _partition(std::move(pv._partition))
{
    if (_backref) {
        _backref->_version = this;
    }
    pv._backref = nullptr;
}

partition_version& partition_version::operator=(partition_version&& pv) noexcept
{
    if (this != &pv) {
        this->~partition_version();
        new (this) partition_version(std::move(pv));
    }
    return *this;
}

partition_version::~partition_version()
{
    if (_backref) {
        _backref->_version = nullptr;
    }
}

stop_iteration partition_version::clear_gently(cache_tracker* tracker) noexcept {
    return _partition.clear_gently(tracker);
}

size_t partition_version::size_in_allocator(const schema& s, allocation_strategy& allocator) const {
    return allocator.object_memory_size_in_allocator(this) +
           partition().external_memory_usage(s);
}

namespace {

// A functor which transforms objects from Domain into objects from CoDomain
template<typename U, typename Domain, typename CoDomain>
concept Mapper =
    requires(U obj, const Domain& src) {
        { obj(src) } -> std::convertible_to<const CoDomain&>;
    };

// A functor which merges two objects from Domain into one. The result is stored in the first argument.
template<typename U, typename Domain>
concept Reducer =
    requires(U obj, Domain& dst, const Domain& src) {
        { obj(dst, src) } -> std::same_as<void>;
    };

// Calculates the value of particular part of mutation_partition represented by
// the version chain starting from v.
// |map| extracts the part from each version.
// |reduce| Combines parts from the two versions.
template <typename Result, typename Map, typename Initial, typename Reduce>
requires Mapper<Map, mutation_partition, Result> && Reducer<Reduce, Result>
inline Result squashed(const partition_version_ref& v, Map&& map, Initial&& initial, Reduce&& reduce) {
    const partition_version* this_v = &*v;
    partition_version* it = v->last();
    Result r = initial(map(it->partition()));
    while (it != this_v) {
        it = it->prev();
        reduce(r, map(it->partition()));
    }
    return r;
}

template <typename Result, typename Map, typename Reduce>
requires Mapper<Map, mutation_partition, Result> && Reducer<Reduce, Result>
inline Result squashed(const partition_version_ref& v, Map&& map, Reduce&& reduce) {
    return squashed<Result>(v, map,
                            [] (auto&& o) -> decltype(auto) { return std::forward<decltype(o)>(o); },
                            reduce);
}

}

::static_row partition_snapshot::static_row(bool digest_requested) const {
    return ::static_row(::squashed<row>(version(),
                         [&] (const mutation_partition& mp) -> const row& {
                            if (digest_requested) {
                                mp.static_row().prepare_hash(*_schema, column_kind::static_column);
                            }
                            return mp.static_row().get();
                         },
                         [this] (const row& r) { return row(*_schema, column_kind::static_column, r); },
                         [this] (row& a, const row& b) { a.apply(*_schema, column_kind::static_column, b); }));
}

bool partition_snapshot::static_row_continuous() const {
    return version()->partition().static_row_continuous();
}

tombstone partition_snapshot::partition_tombstone() const {
    return ::squashed<tombstone>(version(),
                               [] (const mutation_partition& mp) { return mp.partition_tombstone(); },
                               [] (tombstone& a, tombstone b) { a.apply(b); });
}

mutation_partition partition_snapshot::squashed() const {
    return ::squashed<mutation_partition>(version(),
                               [] (const mutation_partition& mp) -> const mutation_partition& { return mp; },
                               [this] (const mutation_partition& mp) { return mutation_partition(*_schema, mp); },
                               [this] (mutation_partition& a, const mutation_partition& b) {
                                   mutation_application_stats app_stats;
                                   a.apply(*_schema, b, *_schema, app_stats);
                               });
}

tombstone partition_entry::partition_tombstone() const {
    return ::squashed<tombstone>(_version,
        [] (const mutation_partition& mp) { return mp.partition_tombstone(); },
        [] (tombstone& a, tombstone b) { a.apply(b); });
}

partition_snapshot::~partition_snapshot() {
    with_allocator(region().allocator(), [this] {
        if (_locked) {
            touch();
        }
        if (_version && _version.is_unique_owner()) {
            auto v = &*_version;
            _version = {};
            remove_or_mark_as_unique_owner(v, _cleaner);
        } else if (_entry) {
            _entry->_snapshot = nullptr;
        }
    });
}

void merge_versions(const schema& s, mutation_partition& newer, mutation_partition&& older, cache_tracker* tracker) {
    mutation_application_stats app_stats;
    older.apply_monotonically(s, std::move(newer), tracker, app_stats);
    newer = std::move(older);
}

stop_iteration partition_snapshot::merge_partition_versions(mutation_application_stats& app_stats) {
    partition_version_ref& v = version();
    if (!v.is_unique_owner()) {
        // Shift _version to the oldest unreferenced version and then keep merging left hand side into it.
        // This is good for performance because in case we were at the latest version
        // we leave it for incoming writes and they don't have to create a new one.
        partition_version* current = &*v;
        while (current->next() && !current->next()->is_referenced()) {
            current = current->next();
            _version = partition_version_ref(*current);
        }
        while (auto prev = current->prev()) {
            region().allocator().invalidate_references();
            // Here we count writes that overwrote rows from a previous version. Total number of writes does not change.
            mutation_application_stats local_app_stats;
            const auto do_stop_iteration = current->partition().apply_monotonically(*schema(),
                    std::move(prev->partition()), _tracker, local_app_stats, is_preemptible::yes);
            app_stats.row_hits += local_app_stats.row_hits;
            if (do_stop_iteration == stop_iteration::no) {
                return stop_iteration::no;
            }
            if (prev->is_referenced()) {
                _version.release();
                prev->back_reference() = partition_version_ref(*current, prev->back_reference().is_unique_owner());
                current_allocator().destroy(prev);
                return stop_iteration::yes;
            }
            current_allocator().destroy(prev);
        }
    }
    return stop_iteration::yes;
}

stop_iteration partition_snapshot::slide_to_oldest() noexcept {
    partition_version_ref& v = version();
    if (v.is_unique_owner()) {
        return stop_iteration::yes;
    }
    if (_entry) {
        _entry->_snapshot = nullptr;
        _entry = nullptr;
    }
    partition_version* current = &*v;
    while (current->next() && !current->next()->is_referenced()) {
        current = current->next();
        _version = partition_version_ref(*current);
    }
    return current->prev() ? stop_iteration::no : stop_iteration::yes;
}

unsigned partition_snapshot::version_count()
{
    unsigned count = 0;
    for (auto&& v : versions()) {
        (void)v;
        count++;
    }
    return count;
}

partition_entry::partition_entry(mutation_partition mp)
{
    auto new_version = current_allocator().construct<partition_version>(std::move(mp));
    _version = partition_version_ref(*new_version);
}

partition_entry::partition_entry(partition_entry::evictable_tag, const schema& s, mutation_partition&& mp)
    : partition_entry([&] {
        mp.ensure_last_dummy(s);
        return std::move(mp);
    }())
{ }

partition_entry partition_entry::make_evictable(const schema& s, mutation_partition&& mp) {
    return {evictable_tag(), s, std::move(mp)};
}

partition_entry partition_entry::make_evictable(const schema& s, const mutation_partition& mp) {
    return make_evictable(s, mutation_partition(s, mp));
}

partition_entry::~partition_entry() {
    if (!_version) {
        return;
    }
    if (_snapshot) {
        assert(!_snapshot->is_locked());
        _snapshot->_version = std::move(_version);
        _snapshot->_version.mark_as_unique_owner();
        _snapshot->_entry = nullptr;
    } else {
        auto v = &*_version;
        _version = { };
        remove_or_mark_as_unique_owner(v, no_cleaner);
    }
}

stop_iteration partition_entry::clear_gently(cache_tracker* tracker) noexcept {
    if (!_version) {
        return stop_iteration::yes;
    }

    if (_snapshot) {
        assert(!_snapshot->is_locked());
        _snapshot->_version = std::move(_version);
        _snapshot->_version.mark_as_unique_owner();
        _snapshot->_entry = nullptr;
        return stop_iteration::yes;
    }

    partition_version* v = &*_version;
    _version = {};
    while (v) {
        if (v->is_referenced()) {
            v->back_reference().mark_as_unique_owner();
            break;
        }
        auto next = v->next();
        if (v->clear_gently(tracker) == stop_iteration::no) {
            _version = partition_version_ref(*v);
            return stop_iteration::no;
        }
        current_allocator().destroy(&*v);
        v = next;
    }
    return stop_iteration::yes;
}

void partition_entry::set_version(partition_version* new_version)
{
    if (_snapshot) {
        assert(!_snapshot->is_locked());
        _snapshot->_version = std::move(_version);
        _snapshot->_entry = nullptr;
    }

    _snapshot = nullptr;
    _version = partition_version_ref(*new_version);
}

partition_version& partition_entry::add_version(const schema& s, cache_tracker* tracker) {
    // Every evictable version must have a dummy entry at the end so that
    // it can be tracked in the LRU. It is also needed to allow old versions
    // to stay around (with tombstones and static rows) after fully evicted.
    // Such versions must be fully discontinuous, and thus have a dummy at the end.
    auto new_version = tracker
                       ? current_allocator().construct<partition_version>(mutation_partition::make_incomplete(s))
                       : current_allocator().construct<partition_version>(mutation_partition(s.shared_from_this()));
    new_version->partition().set_static_row_continuous(_version->partition().static_row_continuous());
    new_version->insert_before(*_version);
    set_version(new_version);
    if (tracker) {
        tracker->insert(*new_version);
    }
    return *new_version;
}

void partition_entry::apply(const schema& s, const mutation_partition& mp, const schema& mp_schema,
        mutation_application_stats& app_stats) {
    apply(s, mutation_partition(mp_schema, mp), mp_schema, app_stats);
}

void partition_entry::apply(const schema& s, mutation_partition&& mp, const schema& mp_schema,
        mutation_application_stats& app_stats) {
    // A note about app_stats: it may happen that mp has rows that overwrite other rows
    // in older partition_version. Those overwrites will be counted when their versions get merged.
    if (s.version() != mp_schema.version()) {
        mp.upgrade(mp_schema, s);
    }
    auto new_version = current_allocator().construct<partition_version>(std::move(mp));
    if (!_snapshot) {
        try {
            _version->partition().apply_monotonically(s, std::move(new_version->partition()), no_cache_tracker, app_stats);
            current_allocator().destroy(new_version);
            return;
        } catch (...) {
            // fall through
        }
    }
    new_version->insert_before(*_version);
    set_version(new_version);
    app_stats.row_writes += new_version->partition().row_count();
}

coroutine partition_entry::apply_to_incomplete(const schema& s,
    partition_entry&& pe,
    mutation_cleaner& pe_cleaner,
    logalloc::allocating_section& alloc,
    logalloc::region& reg,
    cache_tracker& tracker,
    partition_snapshot::phase_type phase,
    real_dirty_memory_accounter& acc)
{
    // This flag controls whether this operation may defer. It is more
    // expensive to apply with deferring due to construction of snapshots and
    // two-pass application, with the first pass filtering and moving data to
    // the new version and the second pass merging it back once all is done.
    // We cannot merge into current version because if we defer in the middle
    // that may publish partial writes. Also, snapshot construction results in
    // creation of garbage objects, partition_version and rows_entry. Garbage
    // will yield sparse segments and add overhead due to increased LSA
    // segment compaction. This becomes especially significant for small
    // partitions where I saw 40% slow down.
    const bool preemptible = s.clustering_key_size() > 0;

    // When preemptible, later memtable reads could start using the snapshot before
    // snapshot's writes are made visible in cache, which would cause them to miss those writes.
    // So we cannot allow erasing when preemptible.
    bool can_move = !preemptible && !pe._snapshot;

    auto src_snp = pe.read(reg, pe_cleaner, s.shared_from_this(), no_cache_tracker);
    partition_snapshot_ptr prev_snp;
    if (preemptible) {
        // Reads must see prev_snp until whole update completes so that writes
        // are not partially visible.
        prev_snp = read(reg, tracker.cleaner(), s.shared_from_this(), &tracker, phase - 1);
    }
    auto dst_snp = read(reg, tracker.cleaner(), s.shared_from_this(), &tracker, phase);
    dst_snp->lock();

    // Once we start updating the partition, we must keep all snapshots until the update completes,
    // otherwise partial writes would be published. So the scope of snapshots must enclose the scope
    // of allocating sections, so we return here to get out of the current allocating section and
    // give the caller a chance to store the coroutine object. The code inside coroutine below
    // runs outside allocating section.
    return coroutine([&tracker, &s, &alloc, &reg, &acc, can_move, preemptible,
            cur = partition_snapshot_row_cursor(s, *dst_snp),
            src_cur = partition_snapshot_row_cursor(s, *src_snp, can_move),
            dst_snp = std::move(dst_snp),
            prev_snp = std::move(prev_snp),
            src_snp = std::move(src_snp),
            static_done = false] () mutable {
        auto&& allocator = reg.allocator();
        return alloc(reg, [&] {
            return with_linearized_managed_bytes([&] {
                size_t dirty_size = 0;

                if (!static_done) {
                    partition_version& dst = *dst_snp->version();
                    bool static_row_continuous = dst_snp->static_row_continuous();
                    auto current = &*src_snp->version();
                    while (current) {
                        dirty_size += allocator.object_memory_size_in_allocator(current)
                            + current->partition().static_row().external_memory_usage(s, column_kind::static_column);
                        dst.partition().apply(current->partition().partition_tombstone());
                        if (static_row_continuous) {
                            lazy_row& static_row = dst.partition().static_row();
                            if (can_move) {
                                static_row.apply(s, column_kind::static_column,
                                    std::move(current->partition().static_row()));
                            } else {
                                static_row.apply(s, column_kind::static_column, current->partition().static_row());
                            }
                        }
                        dirty_size += current->partition().row_tombstones().external_memory_usage(s);
                        range_tombstone_list& tombstones = dst.partition().row_tombstones();
                        // FIXME: defer while applying range tombstones
                        if (can_move) {
                            tombstones.apply_monotonically(s, std::move(current->partition().row_tombstones()));
                        } else {
                            tombstones.apply_monotonically(s, current->partition().row_tombstones());
                        }
                        current = current->next();
                        can_move &= current && !current->is_referenced();
                    }
                    acc.unpin_memory(dirty_size);
                    static_done = true;
                }

                if (!src_cur.maybe_refresh_static()) {
                    return stop_iteration::yes;
                }

                do {
                    auto size = src_cur.memory_usage();
                    if (!src_cur.dummy()) {
                        tracker.on_row_processed_from_memtable();
                        auto ropt = cur.ensure_entry_if_complete(src_cur.position());
                        if (ropt) {
                            if (!ropt->inserted) {
                                tracker.on_row_merged_from_memtable();
                            }
                            rows_entry& e = ropt->row;
                            src_cur.consume_row([&](deletable_row&& row) {
                                e.row().apply_monotonically(s, std::move(row));
                            });
                        } else {
                            tracker.on_row_dropped_from_memtable();
                        }
                    }
                    auto has_next = src_cur.erase_and_advance();
                    acc.unpin_memory(size);
                    if (!has_next) {
                        dst_snp->unlock();
                        return stop_iteration::yes;
                    }
                } while (!preemptible || !need_preempt());
                return stop_iteration::no;
            });
        });
    });
}

mutation_partition partition_entry::squashed(schema_ptr from, schema_ptr to)
{
    mutation_partition mp(to);
    mp.set_static_row_continuous(_version->partition().static_row_continuous());
    for (auto&& v : _version->all_elements()) {
        auto older = mutation_partition(*from, v.partition());
        if (from->version() != to->version()) {
            older.upgrade(*from, *to);
        }
        merge_versions(*to, mp, std::move(older), no_cache_tracker);
    }
    return mp;
}

mutation_partition partition_entry::squashed(const schema& s)
{
    return squashed(s.shared_from_this(), s.shared_from_this());
}

void partition_entry::upgrade(schema_ptr from, schema_ptr to, mutation_cleaner& cleaner, cache_tracker* tracker)
{
    auto new_version = current_allocator().construct<partition_version>(squashed(from, to));
    auto old_version = &*_version;
    set_version(new_version);
    if (tracker) {
        tracker->insert(*new_version);
    }
    remove_or_mark_as_unique_owner(old_version, &cleaner);
}

partition_snapshot_ptr partition_entry::read(logalloc::region& r,
    mutation_cleaner& cleaner, schema_ptr entry_schema, cache_tracker* tracker, partition_snapshot::phase_type phase)
{
    if (_snapshot) {
        if (_snapshot->_phase == phase) {
            return _snapshot->shared_from_this();
        } else if (phase < _snapshot->_phase) {
            // If entry is being updated, we will get reads for non-latest phase, and
            // they must attach to the non-current version.
            partition_version* second = _version->next();
            assert(second && second->is_referenced());
            auto snp = partition_snapshot::container_of(second->_backref).shared_from_this();
            assert(phase == snp->_phase);
            return snp;
        } else { // phase > _snapshot->_phase
            with_allocator(r.allocator(), [&] {
                add_version(*entry_schema, tracker);
            });
        }
    }

    auto snp = make_lw_shared<partition_snapshot>(entry_schema, r, cleaner, this, tracker, phase);
    _snapshot = snp.get();
    return partition_snapshot_ptr(std::move(snp));
}

partition_snapshot::range_tombstone_result
partition_snapshot::range_tombstones(position_in_partition_view start, position_in_partition_view end)
{
    partition_version* v = &*version();
    if (!v->next()) {
        return boost::copy_range<range_tombstone_result>(
            v->partition().row_tombstones().slice(*_schema, start, end));
    }
    range_tombstone_list list(*_schema);
    while (v) {
        for (auto&& rt : v->partition().row_tombstones().slice(*_schema, start, end)) {
            list.apply(*_schema, rt);
        }
        v = v->next();
    }
    return boost::copy_range<range_tombstone_result>(list.slice(*_schema, start, end));
}

partition_snapshot::range_tombstone_result
partition_snapshot::range_tombstones()
{
    return range_tombstones(
        position_in_partition_view::before_all_clustered_rows(),
        position_in_partition_view::after_all_clustered_rows());
}

void partition_snapshot::touch() noexcept {
    // Eviction assumes that older versions are evicted before newer so only the latest snapshot
    // can be touched.
    if (_tracker && at_latest_version()) {
        auto&& rows = version()->partition().clustered_rows();
        assert(!rows.empty());
        rows_entry& last_dummy = *rows.rbegin();
        assert(last_dummy.is_last_dummy());
        _tracker->touch(last_dummy);
    }
}

std::ostream& operator<<(std::ostream& out, const partition_entry::printer& p) {
    auto& e = p._partition_entry;
    out << "{";
    bool first = true;
    if (e._version) {
        const partition_version* v = &*e._version;
        while (v) {
            if (!first) {
                out << ", ";
            }
            if (v->is_referenced()) {
                out << "(*) ";
            }
            out << mutation_partition::printer(p._schema, v->partition());
            v = v->next();
            first = false;
        }
    }
    out << "}";
    return out;
}

void partition_entry::evict(mutation_cleaner& cleaner) noexcept {
    if (!_version) {
        return;
    }
    if (_snapshot) {
        assert(!_snapshot->is_locked());
        _snapshot->_version = std::move(_version);
        _snapshot->_version.mark_as_unique_owner();
        _snapshot->_entry = nullptr;
    } else {
        auto v = &*_version;
        _version = { };
        remove_or_mark_as_unique_owner(v, &cleaner);
    }
}

partition_snapshot_ptr::~partition_snapshot_ptr() {
    if (_snp) {
        auto&& cleaner = _snp->cleaner();
        auto snp = _snp.release();
        if (snp) {
            cleaner.merge_and_destroy(*snp.release());
        }
    }
}

void partition_snapshot::lock() noexcept {
    // partition_entry::is_locked() assumes that if there is a locked snapshot,
    // it can be found attached directly to it.
    assert(at_latest_version());
    _locked = true;
}

void partition_snapshot::unlock() noexcept {
    // Locked snapshots must always be latest, is_locked() assumes that.
    // Also, touch() is only effective when this snapshot is latest. 
    assert(at_latest_version());
    _locked = false;
    touch(); // Make the entry evictable again in case it was fully unlinked by eviction attempt.
}
