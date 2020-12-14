/*
 * Copyright 2015 ScyllaDB
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

#include "schema_mutations.hh"
#include "canonical_mutation.hh"
#include "db/schema_tables.hh"
#include "hashers.hh"

schema_mutations::schema_mutations(canonical_mutation columnfamilies,
                                   canonical_mutation columns,
                                   bool is_view,
                                   std::optional<canonical_mutation> indices,
                                   std::optional<canonical_mutation> dropped_columns,
                                   std::optional<canonical_mutation> scylla_tables,
                                   std::optional<canonical_mutation> view_virtual_columns,
                                   std::optional<canonical_mutation> computed_columns)
    : _columnfamilies(columnfamilies.to_mutation(is_view ? db::schema_tables::views() : db::schema_tables::tables()))
    , _columns(columns.to_mutation(db::schema_tables::columns()))
    , _view_virtual_columns(view_virtual_columns ? mutation_opt{view_virtual_columns.value().to_mutation(db::schema_tables::view_virtual_columns())} : std::nullopt)
    , _computed_columns(computed_columns ? mutation_opt{computed_columns.value().to_mutation(db::schema_tables::computed_columns())} : std::nullopt)
    , _indices(indices ? mutation_opt{indices.value().to_mutation(db::schema_tables::indexes())} : std::nullopt)
    , _dropped_columns(dropped_columns ? mutation_opt{dropped_columns.value().to_mutation(db::schema_tables::dropped_columns())} : std::nullopt)
    , _scylla_tables(scylla_tables ? mutation_opt{scylla_tables.value().to_mutation(db::schema_tables::scylla_tables())} : std::nullopt)
{}

void schema_mutations::copy_to(std::vector<mutation>& dst) const {
    dst.push_back(_columnfamilies);
    dst.push_back(_columns);
    if (_view_virtual_columns) {
        dst.push_back(*_view_virtual_columns);
    }
    if (_computed_columns) {
        dst.push_back(*_computed_columns);
    }
    if (_indices) {
        dst.push_back(*_indices);
    }
    if (_dropped_columns) {
        dst.push_back(*_dropped_columns);
    }
    if (_scylla_tables) {
        dst.push_back(*_scylla_tables);
    }
}

table_schema_version schema_mutations::digest() const {
    if (_scylla_tables) {
        auto rs = query::result_set(*_scylla_tables);
        if (!rs.empty()) {
            auto&& row = rs.row(0);
            auto val = row.get<utils::UUID>("version");
            if (val) {
                return *val;
            }
        }
    }

    md5_hasher h;
    db::schema_features sf = db::schema_features::full();

    // Disable this feature so that the digest remains compactible with Scylla
    // versions prior to this feature.
    // This digest affects the table schema version calculation and it's important
    // that all nodes arrive at the same table schema version to avoid needless schema version
    // pulls. Table schema versions are calculated on boot when we don't yet
    // know all the cluster features, so we could get different table versions after reboot
    // in an already upgraded cluster.
    sf.remove<db::schema_feature::DIGEST_INSENSITIVE_TO_EXPIRY>();

    db::schema_tables::feed_hash_for_schema_digest(h, _columnfamilies, sf);
    db::schema_tables::feed_hash_for_schema_digest(h, _columns, sf);
    if (_view_virtual_columns && !_view_virtual_columns->partition().empty()) {
        db::schema_tables::feed_hash_for_schema_digest(h, *_view_virtual_columns, sf);
    }
    if (_computed_columns && !_computed_columns->partition().empty()) {
        db::schema_tables::feed_hash_for_schema_digest(h, *_computed_columns, sf);
    }
    if (_indices && !_indices->partition().empty()) {
        db::schema_tables::feed_hash_for_schema_digest(h, *_indices, sf);
    }
    if (_dropped_columns && !_dropped_columns->partition().empty()) {
        db::schema_tables::feed_hash_for_schema_digest(h, *_dropped_columns, sf);
    }
    if (_scylla_tables) {
        db::schema_tables::feed_hash_for_schema_digest(h, *_scylla_tables, sf);
    }
    return utils::UUID_gen::get_name_UUID(h.finalize());
}

std::optional<sstring> schema_mutations::partitioner() const {
    if (_scylla_tables) {
        auto rs = query::result_set(*_scylla_tables);
        if (!rs.empty()) {
            return rs.row(0).get<sstring>("partitioner");
        }
    }
    return { };
}

static mutation_opt compact(const mutation_opt& m) {
    if (!m) {
        return m;
    }
    return db::schema_tables::compact_for_schema_digest(*m);
}

static mutation_opt compact(const mutation& m) {
    return db::schema_tables::compact_for_schema_digest(m);
}

bool schema_mutations::operator==(const schema_mutations& other) const {
    return compact(_columnfamilies) == compact(other._columnfamilies)
           && compact(_columns) == compact(other._columns)
           && compact(_view_virtual_columns) == compact(other._view_virtual_columns)
           && compact(_computed_columns) == compact(other._computed_columns)
           && compact(_indices) == compact(other._indices)
           && compact(_dropped_columns) == compact(other._dropped_columns)
           && compact(_scylla_tables) == compact(other._scylla_tables)
           ;
}

bool schema_mutations::operator!=(const schema_mutations& other) const {
    return !(*this == other);
}

bool schema_mutations::live() const {
    return _columnfamilies.live_row_count() > 0 || _columns.live_row_count() > 0 ||
            (_view_virtual_columns && _view_virtual_columns->live_row_count() > 0) ||
            (_computed_columns && _computed_columns->live_row_count() > 0);
}

bool schema_mutations::is_view() const {
    return _columnfamilies.schema() == db::schema_tables::views();
}

std::ostream& operator<<(std::ostream& out, const schema_mutations& sm) {
    out << "schema_mutations{\n";
    out << " tables=" << sm.columnfamilies_mutation() << ",\n";
    out << " scylla_tables=" << sm.scylla_tables() << ",\n";
    out << " columns=" << sm.columns_mutation() << ",\n";
    out << " dropped_columns=" << sm.dropped_columns_mutation() << ",\n";
    out << " indices=" << sm.indices_mutation() << "\n";
    out << "}";
    return out;
}
