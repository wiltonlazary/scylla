/*
 * Copyright (C) 2018 ScyllaDB
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


#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/bool_class.hh>

#include "mutation_fragment.hh"
#include "test/lib/mutation_source_test.hh"
#include "flat_mutation_reader.hh"
#include "mutation_writer/multishard_writer.hh"
#include "mutation_writer/timestamp_based_splitting_writer.hh"
#include "test/lib/cql_test_env.hh"
#include "test/lib/mutation_assertions.hh"
#include "test/lib/random_utils.hh"
#include "test/lib/random_schema.hh"
#include "test/lib/log.hh"

using namespace mutation_writer;

struct generate_error_tag { };
using generate_error = bool_class<generate_error_tag>;


constexpr unsigned many_partitions() {
    return
#ifndef SEASTAR_DEBUG
	300
#else
	10
#endif
	;
}

SEASTAR_TEST_CASE(test_multishard_writer) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        auto test_random_streams = [] (random_mutation_generator&& gen, size_t partition_nr, generate_error error = generate_error::no) {
            for (auto i = 0; i < 3; i++) {
                auto muts = gen(partition_nr);
                std::vector<size_t> shards_before(smp::count, 0);
                std::vector<size_t> shards_after(smp::count, 0);
                schema_ptr s = gen.schema();

                for (auto& m : muts) {
                    auto shard = s->get_sharder().shard_of(m.token());
                    shards_before[shard]++;
                }
                auto source_reader = partition_nr > 0 ? flat_mutation_reader_from_mutations(tests::make_permit(), muts) : make_empty_flat_reader(s, tests::make_permit());
                auto& sharder = s->get_sharder();
                size_t partitions_received = distribute_reader_and_consume_on_shards(s,
                    std::move(source_reader),
                    [&sharder, &shards_after, error] (flat_mutation_reader reader) mutable {
                        if (error) {
                            return make_exception_future<>(std::runtime_error("Failed to write"));
                        }
                        return repeat([&sharder, &shards_after, reader = std::move(reader), error] () mutable {
                            return reader(db::no_timeout).then([&sharder, &shards_after, error] (mutation_fragment_opt mf_opt) mutable {
                                if (mf_opt) {
                                    if (mf_opt->is_partition_start()) {
                                        auto shard = sharder.shard_of(mf_opt->as_partition_start().key().token());
                                        BOOST_REQUIRE_EQUAL(shard, this_shard_id());
                                        shards_after[shard]++;
                                    }
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                } else {
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                }
                            });
                        });
                    }
                ).get0();
                BOOST_REQUIRE_EQUAL(partitions_received, partition_nr);
                BOOST_REQUIRE_EQUAL(shards_after, shards_before);
            }
        };

        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::no), 0);
        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::yes, local_shard_only::no), 0);

        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::no), 1);
        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::yes, local_shard_only::no), 1);

        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::no), many_partitions());
        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::yes, local_shard_only::no), many_partitions());

        try {
            test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::no), many_partitions(), generate_error::yes);
            BOOST_ASSERT(false);
        } catch (...) {
        }

        try {
            test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::yes, local_shard_only::no), many_partitions(), generate_error::yes);
            BOOST_ASSERT(false);
        } catch (...) {
        }
    });
}

SEASTAR_TEST_CASE(test_multishard_writer_producer_aborts) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        auto test_random_streams = [] (random_mutation_generator&& gen, size_t partition_nr, generate_error error = generate_error::no) {
            auto muts = gen(partition_nr);
            schema_ptr s = gen.schema();
            auto source_reader = partition_nr > 0 ? flat_mutation_reader_from_mutations(tests::make_permit(), muts) : make_empty_flat_reader(s, tests::make_permit());
            int mf_produced = 0;
            auto get_next_mutation_fragment = [&source_reader, &mf_produced] () mutable {
                if (mf_produced++ > 800) {
                    return make_exception_future<mutation_fragment_opt>(std::runtime_error("the producer failed"));
                } else {
                    return source_reader(db::no_timeout);
                }
            };
            auto& sharder = s->get_sharder();
            try {
                distribute_reader_and_consume_on_shards(s,
                    make_generating_reader(s, tests::make_permit(), std::move(get_next_mutation_fragment)),
                    [&sharder, error] (flat_mutation_reader reader) mutable {
                        if (error) {
                            return make_exception_future<>(std::runtime_error("Failed to write"));
                        }
                        return repeat([&sharder, reader = std::move(reader), error] () mutable {
                            return reader(db::no_timeout).then([&sharder,  error] (mutation_fragment_opt mf_opt) mutable {
                                if (mf_opt) {
                                    if (mf_opt->is_partition_start()) {
                                        auto shard = sharder.shard_of(mf_opt->as_partition_start().key().token());
                                        BOOST_REQUIRE_EQUAL(shard, this_shard_id());
                                    }
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                } else {
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                }
                            });
                        });
                    }
                ).get0();
            } catch (...) {
                // The distribute_reader_and_consume_on_shards is expected to fail and not block forever
            }
        };

        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::yes), 1000, generate_error::no);
        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no, local_shard_only::yes), 1000, generate_error::yes);
    });
}

namespace {

class bucket_writer {
    schema_ptr _schema;
    classify_by_timestamp _classify;
    std::unordered_map<int64_t, std::vector<mutation>>& _buckets;

    std::optional<int64_t> _bucket_id;
    mutation_opt _current_mutation;
    bool _is_first_mutation = true;

private:
    void check_timestamp(api::timestamp_type ts) {
        const auto bucket_id = _classify(ts);
        if (_bucket_id) {
            BOOST_REQUIRE_EQUAL(bucket_id, *_bucket_id);
        } else {
            _bucket_id = bucket_id;
        }
    }
    void verify_column_bucket_id(const atomic_cell_or_collection& cell, const column_definition& cdef) {
        if (cdef.is_atomic()) {
            check_timestamp(cell.as_atomic_cell(cdef).timestamp());
        } else if (cdef.type->is_collection() || cdef.type->is_user_type()) {
            cell.as_collection_mutation().with_deserialized(*cdef.type, [this] (collection_mutation_view_description mv) {
                for (const auto& c: mv.cells) {
                    check_timestamp(c.second.timestamp());
                }
            });
        } else {
            BOOST_FAIL(fmt::format("Failed to verify column bucket id: column {} is of unknown type {}", cdef.name_as_text(), cdef.type->name()));
        }
    }
    void verify_row_bucket_id(const row& r, column_kind kind) {
        r.for_each_cell([this, kind] (column_id id, const atomic_cell_or_collection& cell) {
            verify_column_bucket_id(cell, _schema->column_at(kind, id));
        });
    }
    void verify_partition_tombstone(tombstone tomb) {
        if (tomb) {
            check_timestamp(tomb.timestamp);
        }
    }
    void verify_static_row(const static_row& sr) {
        verify_row_bucket_id(sr.cells(), column_kind::static_column);
    }
    void verify_clustering_row(const clustering_row& cr) {
        if (!cr.marker().is_missing()) {
            check_timestamp(cr.marker().timestamp());
        }
        if (cr.tomb()) {
            check_timestamp(cr.tomb().tomb().timestamp);
        }
        verify_row_bucket_id(cr.cells(), column_kind::regular_column);
    }
    void verify_range_tombstone(const range_tombstone& rt) {
        check_timestamp(rt.tomb.timestamp);
    }

public:
    bucket_writer(schema_ptr schema, classify_by_timestamp classify, std::unordered_map<int64_t, std::vector<mutation>>& buckets)
        : _schema(std::move(schema))
        , _classify(std::move(classify))
        , _buckets(buckets) {
    }
    void consume_new_partition(const dht::decorated_key& dk) {
        BOOST_REQUIRE(!_current_mutation);
        _current_mutation = mutation(_schema, dk);
    }
    void consume(tombstone partition_tombstone) {
        BOOST_REQUIRE(_current_mutation);
        verify_partition_tombstone(partition_tombstone);
        _current_mutation->partition().apply(partition_tombstone);
    }
    stop_iteration consume(static_row&& sr) {
        BOOST_REQUIRE(_current_mutation);
        verify_static_row(sr);
        _current_mutation->apply(mutation_fragment(*_schema, tests::make_permit(), std::move(sr)));
        return stop_iteration::no;
    }
    stop_iteration consume(clustering_row&& cr) {
        BOOST_REQUIRE(_current_mutation);
        verify_clustering_row(cr);
        _current_mutation->apply(mutation_fragment(*_schema, tests::make_permit(), std::move(cr)));
        return stop_iteration::no;
    }
    stop_iteration consume(range_tombstone&& rt) {
        BOOST_REQUIRE(_current_mutation);
        verify_range_tombstone(rt);
        _current_mutation->apply(mutation_fragment(*_schema, tests::make_permit(), std::move(rt)));
        return stop_iteration::no;
    }
    stop_iteration consume_end_of_partition() {
        BOOST_REQUIRE(_current_mutation);
        BOOST_REQUIRE(_bucket_id);
        auto& bucket = _buckets[*_bucket_id];

        if (_is_first_mutation) {
            BOOST_REQUIRE(bucket.empty());
            _is_first_mutation = false;
        }

        bucket.emplace_back(std::move(*_current_mutation));
        _current_mutation = std::nullopt;
        return stop_iteration::no;
    }
    void consume_end_of_stream() {
        BOOST_REQUIRE(!_current_mutation);
    }
};

} // anonymous namespace

SEASTAR_THREAD_TEST_CASE(test_timestamp_based_splitting_mutation_writer) {
    auto random_spec = tests::make_random_schema_specification(
            get_name(),
            std::uniform_int_distribution<size_t>(1, 4),
            std::uniform_int_distribution<size_t>(2, 4),
            std::uniform_int_distribution<size_t>(2, 8),
            std::uniform_int_distribution<size_t>(2, 8));
    auto random_schema = tests::random_schema{tests::random::get_int<uint32_t>(), *random_spec};

    testlog.info("Random schema:\n{}", random_schema.cql());

    auto ts_gen = [&, underlying = tests::default_timestamp_generator()] (std::mt19937& engine,
            tests::timestamp_destination ts_dest, api::timestamp_type min_timestamp) -> api::timestamp_type {
        if (ts_dest == tests::timestamp_destination::partition_tombstone ||
                ts_dest == tests::timestamp_destination::row_marker ||
                ts_dest == tests::timestamp_destination::row_tombstone ||
                ts_dest == tests::timestamp_destination::collection_tombstone) {
            if (tests::random::get_int<int>(0, 10, engine)) {
                return api::missing_timestamp;
            }
        }
        return underlying(engine, ts_dest, min_timestamp);
    };

    auto muts = tests::generate_random_mutations(random_schema, ts_gen).get0();

    auto classify_fn = [] (api::timestamp_type ts) {
        return int64_t(ts % 2);
    };

    std::unordered_map<int64_t, std::vector<mutation>> buckets;

    auto consumer = [&] (flat_mutation_reader bucket_reader) {
        return do_with(std::move(bucket_reader), [&] (flat_mutation_reader& rd) {
            return rd.consume(bucket_writer(random_schema.schema(), classify_fn, buckets), db::no_timeout);
        });
    };

    segregate_by_timestamp(flat_mutation_reader_from_mutations(tests::make_permit(), muts), classify_fn, std::move(consumer)).get();

    testlog.debug("Data split into {} buckets: {}", buckets.size(), boost::copy_range<std::vector<int64_t>>(buckets | boost::adaptors::map_keys));

    auto bucket_readers = boost::copy_range<std::vector<flat_mutation_reader>>(buckets | boost::adaptors::map_values |
            boost::adaptors::transformed([] (std::vector<mutation> muts) { return flat_mutation_reader_from_mutations(tests::make_permit(), std::move(muts)); }));
    auto reader = make_combined_reader(random_schema.schema(), tests::make_permit(), std::move(bucket_readers), streamed_mutation::forwarding::no,
            mutation_reader::forwarding::no);

    const auto now = gc_clock::now();
    for (auto& m : muts) {
        m.partition().compact_for_compaction(*random_schema.schema(), always_gc, now);
    }

    std::vector<mutation> combined_mutations;
    while (auto m = read_mutation_from_flat_mutation_reader(reader, db::no_timeout).get0()) {
        m->partition().compact_for_compaction(*random_schema.schema(), always_gc, now);
        combined_mutations.emplace_back(std::move(*m));
    }

    BOOST_REQUIRE_EQUAL(combined_mutations.size(), muts.size());
    for (size_t i = 0; i < muts.size(); ++i) {
        testlog.debug("Comparing mutation #{}", i);
        assert_that(combined_mutations[i]).is_equal_to(muts[i]);
    }

}
