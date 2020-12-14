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


#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include "test/lib/cql_test_env.hh"
#include "test/lib/result_set_assertions.hh"
#include "test/lib/reader_permit.hh"
#include "test/lib/log.hh"

#include "database.hh"
#include "partition_slice_builder.hh"
#include "frozen_mutation.hh"
#include "test/lib/mutation_source_test.hh"
#include "schema_registry.hh"
#include "service/migration_manager.hh"
#include "sstables/sstables.hh"
#include "db/config.hh"
#include "db/commitlog/commitlog_replayer.hh"
#include "test/lib/tmpdir.hh"
#include "db/data_listeners.hh"
#include "multishard_mutation_query.hh"

using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_safety_after_truncate) {
    auto cfg = make_shared<db::config>();
    cfg->auto_snapshot.set(false);
    return do_with_cql_env_thread([](cql_test_env& e) {
        e.execute_cql("create table ks.cf (k text, v int, primary key (k));").get();
        auto& db = e.local_db();
        auto s = db.find_schema("ks", "cf");
        dht::partition_range_vector pranges;

        for (uint32_t i = 1; i <= 1000; ++i) {
            auto pkey = partition_key::from_single_value(*s, to_bytes(sprint("key%d", i)));
            mutation m(s, pkey);
            m.set_clustered_cell(clustering_key_prefix::make_empty(), "v", int32_t(42), {});
            pranges.emplace_back(dht::partition_range::make_singular(dht::decorate_key(*s, std::move(pkey))));
            db.apply(s, freeze(m), tracing::trace_state_ptr(), db::commitlog::force_sync::no, db::no_timeout).get();
        }

        auto assert_query_result = [&] (size_t expected_size) {
            auto max_size = std::numeric_limits<size_t>::max();
            auto cmd = query::read_command(s->id(), s->version(), partition_slice_builder(*s).build(), query::max_result_size(max_size), query::row_limit(1000));
            auto&& [result, cache_tempature] = db.query(s, cmd, query::result_options::only_result(), pranges, nullptr, db::no_timeout).get0();
            assert_that(query::result_set::from_raw_result(s, cmd.slice, *result)).has_size(expected_size);
        };
        assert_query_result(1000);

        db.truncate("ks", "cf", [] { return make_ready_future<db_clock::time_point>(db_clock::now()); }).get();

        assert_query_result(0);

        auto cl = db.commitlog();
        auto rp = db::commitlog_replayer::create_replayer(e.db()).get0();
        auto paths = cl->list_existing_segments().get0();
        rp.recover(paths, db::commitlog::descriptor::FILENAME_PREFIX).get();

        assert_query_result(0);
        return make_ready_future<>();
    }, cfg);
}

SEASTAR_TEST_CASE(test_querying_with_limits) {
    return do_with_cql_env([](cql_test_env& e) {
        return seastar::async([&] {
            e.execute_cql("create table ks.cf (k text, v int, primary key (k));").get();
            auto& db = e.local_db();
            auto s = db.find_schema("ks", "cf");
            dht::partition_range_vector pranges;
            for (uint32_t i = 1; i <= 3; ++i) {
                auto pkey = partition_key::from_single_value(*s, to_bytes(format("key{:d}", i)));
                mutation m(s, pkey);
                m.partition().apply(tombstone(api::timestamp_type(1), gc_clock::now()));
                db.apply(s, freeze(m), tracing::trace_state_ptr(), db::commitlog::force_sync::no, db::no_timeout).get();
            }
            for (uint32_t i = 3; i <= 8; ++i) {
                auto pkey = partition_key::from_single_value(*s, to_bytes(format("key{:d}", i)));
                mutation m(s, pkey);
                m.set_clustered_cell(clustering_key_prefix::make_empty(), "v", int32_t(42), 1);
                db.apply(s, freeze(m), tracing::trace_state_ptr(), db::commitlog::force_sync::no, db::no_timeout).get();
                pranges.emplace_back(dht::partition_range::make_singular(dht::decorate_key(*s, std::move(pkey))));
            }

            auto max_size = std::numeric_limits<size_t>::max();
            {
                auto cmd = query::read_command(s->id(), s->version(), partition_slice_builder(*s).build(), query::max_result_size(max_size), query::row_limit(3));
                auto result = std::get<0>(db.query(s, cmd, query::result_options::only_result(), pranges, nullptr, db::no_timeout).get0());
                assert_that(query::result_set::from_raw_result(s, cmd.slice, *result)).has_size(3);
            }

            {
                auto cmd = query::read_command(s->id(), s->version(), partition_slice_builder(*s).build(), query::max_result_size(max_size),
                        query::row_limit(query::max_rows), query::partition_limit(5));
                auto result = std::get<0>(db.query(s, cmd, query::result_options::only_result(), pranges, nullptr, db::no_timeout).get0());
                assert_that(query::result_set::from_raw_result(s, cmd.slice, *result)).has_size(5);
            }

            {
                auto cmd = query::read_command(s->id(), s->version(), partition_slice_builder(*s).build(), query::max_result_size(max_size),
                        query::row_limit(query::max_rows), query::partition_limit(3));
                auto result = std::get<0>(db.query(s, cmd, query::result_options::only_result(), pranges, nullptr, db::no_timeout).get0());
                assert_that(query::result_set::from_raw_result(s, cmd.slice, *result)).has_size(3);
            }
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_database_with_data_in_sstables_is_a_mutation_source) {
    do_with_cql_env_thread([] (cql_test_env& e) {
        run_mutation_source_tests([&] (schema_ptr s, const std::vector<mutation>& partitions) -> mutation_source {
            try {
                e.local_db().find_column_family(s->ks_name(), s->cf_name());
                service::get_local_migration_manager().announce_column_family_drop(s->ks_name(), s->cf_name(), true).get();
            } catch (const no_such_column_family&) {
                // expected
            }
            service::get_local_migration_manager().announce_new_column_family(s, true).get();
            column_family& cf = e.local_db().find_column_family(s);
            for (auto&& m : partitions) {
                e.local_db().apply(cf.schema(), freeze(m), tracing::trace_state_ptr(), db::commitlog::force_sync::no, db::no_timeout).get();
            }
            cf.flush().get();
            cf.get_row_cache().invalidate([] {}).get();
            return mutation_source([&] (schema_ptr s,
                    reader_permit,
                    const dht::partition_range& range,
                    const query::partition_slice& slice,
                    const io_priority_class& pc,
                    tracing::trace_state_ptr trace_state,
                    streamed_mutation::forwarding fwd,
                    mutation_reader::forwarding fwd_mr) {
                return cf.make_reader(s, tests::make_permit(), range, slice, pc, std::move(trace_state), fwd, fwd_mr);
            });
        });
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_distributed_loader_with_incomplete_sstables) {
    using sst = sstables::sstable;

    tmpdir data_dir;
    auto db_cfg_ptr = make_shared<db::config>();
    auto& db_cfg = *db_cfg_ptr;

    db_cfg.data_file_directories({data_dir.path().string()}, db::config::config_source::CommandLine);

    // Create incomplete sstables in test data directory
    sstring ks = "system";
    sstring cf = "peers-37f71aca7dc2383ba70672528af04d4f";
    sstring sst_dir = (data_dir.path() / std::string_view(ks) / std::string_view(cf)).string();

    auto require_exist = [] (const sstring& name, bool should_exist) {
        auto exists = file_exists(name).get0();
        BOOST_REQUIRE(exists == should_exist);
    };

    auto touch_dir = [&require_exist] (const sstring& dir_name) {
        recursive_touch_directory(dir_name).get();
        require_exist(dir_name, true);
    };

    auto touch_file = [&require_exist] (const sstring& file_name) {
        auto f = open_file_dma(file_name, open_flags::create).get0();
        f.close().get();
        require_exist(file_name, true);
    };

    auto temp_sst_dir = sst::temp_sst_dir(sst_dir, 2);
    touch_dir(temp_sst_dir);

    temp_sst_dir = sst::temp_sst_dir(sst_dir, 3);
    touch_dir(temp_sst_dir);
    auto temp_file_name = sst::filename(temp_sst_dir, ks, cf, sst::version_types::mc, 3, sst::format_types::big, component_type::TemporaryTOC);
    touch_file(temp_file_name);

    temp_file_name = sst::filename(sst_dir, ks, cf, sst::version_types::mc, 4, sst::format_types::big, component_type::TemporaryTOC);
    touch_file(temp_file_name);
    temp_file_name = sst::filename(sst_dir, ks, cf, sst::version_types::mc, 4, sst::format_types::big, component_type::Data);
    touch_file(temp_file_name);

    do_with_cql_env_thread([&sst_dir, &ks, &cf, &require_exist] (cql_test_env& e) {
        require_exist(sst::temp_sst_dir(sst_dir, 2), false);
        require_exist(sst::temp_sst_dir(sst_dir, 3), false);

        require_exist(sst::filename(sst_dir, ks, cf, sst::version_types::mc, 4, sst::format_types::big, component_type::TemporaryTOC), false);
        require_exist(sst::filename(sst_dir, ks, cf, sst::version_types::mc, 4, sst::format_types::big, component_type::Data), false);
    }, db_cfg_ptr).get();
}

SEASTAR_THREAD_TEST_CASE(test_distributed_loader_with_pending_delete) {
    using sst = sstables::sstable;

    tmpdir data_dir;
    auto db_cfg_ptr = make_shared<db::config>();
    auto& db_cfg = *db_cfg_ptr;

    db_cfg.data_file_directories({data_dir.path().string()}, db::config::config_source::CommandLine);

    // Create incomplete sstables in test data directory
    sstring ks = "system";
    sstring cf = "peers-37f71aca7dc2383ba70672528af04d4f";
    sstring sst_dir = (data_dir.path() / std::string_view(ks) / std::string_view(cf)).string();
    sstring pending_delete_dir = sst_dir + "/" + sst::pending_delete_dir_basename();

    auto require_exist = [] (const sstring& name, bool should_exist) {
        auto exists = file_exists(name).get0();
        if (should_exist) {
            BOOST_REQUIRE(exists);
        } else {
            BOOST_REQUIRE(!exists);
        }
    };

    auto touch_dir = [&require_exist] (const sstring& dir_name) {
        recursive_touch_directory(dir_name).get();
        require_exist(dir_name, true);
    };

    auto touch_file = [&require_exist] (const sstring& file_name) {
        auto f = open_file_dma(file_name, open_flags::create).get0();
        f.close().get();
        require_exist(file_name, true);
    };

    auto write_file = [&require_exist] (const sstring& file_name, const sstring& text) {
        auto f = open_file_dma(file_name, open_flags::wo | open_flags::create | open_flags::truncate).get0();
        auto os = make_file_output_stream(f, file_output_stream_options{}).get0();
        os.write(text).get();
        os.flush().get();
        os.close().get();
        require_exist(file_name, true);
    };

    auto component_basename = [&ks, &cf] (int64_t gen, component_type ctype) {
        return sst::component_basename(ks, cf, sst::version_types::mc, gen, sst::format_types::big, ctype);
    };

    auto gen_filename = [&sst_dir, &ks, &cf] (int64_t gen, component_type ctype) {
        return sst::filename(sst_dir, ks, cf, sst::version_types::mc, gen, sst::format_types::big, ctype);
    };

    touch_dir(pending_delete_dir);

    // Empty log file
    touch_file(pending_delete_dir + "/sstables-0-0.log");

    // Empty temporary log file
    touch_file(pending_delete_dir + "/sstables-1-1.log.tmp");

    const sstring toc_text = "TOC.txt\nData.db\n";

    // Regular log file with single entry
    write_file(gen_filename(2, component_type::TOC), toc_text);
    touch_file(gen_filename(2, component_type::Data));
    write_file(pending_delete_dir + "/sstables-2-2.log",
               component_basename(2, component_type::TOC) + "\n");

    // Temporary log file with single entry
    write_file(pending_delete_dir + "/sstables-3-3.log.tmp",
               component_basename(3, component_type::TOC) + "\n");

    // Regular log file with multiple entries
    write_file(gen_filename(4, component_type::TOC), toc_text);
    touch_file(gen_filename(4, component_type::Data));
    write_file(gen_filename(5, component_type::TOC), toc_text);
    touch_file(gen_filename(5, component_type::Data));
    write_file(pending_delete_dir + "/sstables-4-5.log",
               component_basename(4, component_type::TOC) + "\n" +
               component_basename(5, component_type::TOC) + "\n");

    // Regular log file with multiple entries and some deleted sstables
    write_file(gen_filename(6, component_type::TemporaryTOC), toc_text);
    touch_file(gen_filename(6, component_type::Data));
    write_file(gen_filename(7, component_type::TemporaryTOC), toc_text);
    write_file(pending_delete_dir + "/sstables-6-8.log",
               component_basename(6, component_type::TOC) + "\n" +
               component_basename(7, component_type::TOC) + "\n" +
               component_basename(8, component_type::TOC) + "\n");

    do_with_cql_env_thread([&] (cql_test_env& e) {
        // Empty log file
        require_exist(pending_delete_dir + "/sstables-0-0.log", false);

        // Empty temporary log file
        require_exist(pending_delete_dir + "/sstables-1-1.log.tmp", false);

        // Regular log file with single entry
        require_exist(gen_filename(2, component_type::TOC), false);
        require_exist(gen_filename(2, component_type::Data), false);
        require_exist(pending_delete_dir + "/sstables-2-2.log", false);

        // Temporary log file with single entry
        require_exist(pending_delete_dir + "/sstables-3-3.log.tmp", false);

        // Regular log file with multiple entries
        require_exist(gen_filename(4, component_type::TOC), false);
        require_exist(gen_filename(4, component_type::Data), false);
        require_exist(gen_filename(5, component_type::TOC), false);
        require_exist(gen_filename(5, component_type::Data), false);
        require_exist(pending_delete_dir + "/sstables-4-5.log", false);

        // Regular log file with multiple entries and some deleted sstables
        require_exist(gen_filename(6, component_type::TemporaryTOC), false);
        require_exist(gen_filename(6, component_type::Data), false);
        require_exist(gen_filename(7, component_type::TemporaryTOC), false);
        require_exist(pending_delete_dir + "/sstables-6-8.log", false);
    }, db_cfg_ptr).get();
}

// Snapshot tests and their helpers
future<> do_with_some_data(std::function<future<> (cql_test_env& env)> func) {
    return seastar::async([func = std::move(func)] () mutable {
        tmpdir tmpdir_for_data;
        auto db_cfg_ptr = make_shared<db::config>();
        db_cfg_ptr->data_file_directories(std::vector<sstring>({ tmpdir_for_data.path().string() }));
        do_with_cql_env_thread([func = std::move(func)] (cql_test_env& e) {
            e.create_table([](std::string_view ks_name) {
                return schema({}, ks_name, "cf",
                              {{"p1", utf8_type}},
                              {{"c1", int32_type}, {"c2", int32_type}},
                              {{"r1", int32_type}},
                              {},
                              utf8_type);
            }).get();
            e.execute_cql("insert into cf (p1, c1, c2, r1) values ('key1', 1, 2, 3);").get();
            e.execute_cql("insert into cf (p1, c1, c2, r1) values ('key1', 2, 2, 3);").get();
            e.execute_cql("insert into cf (p1, c1, c2, r1) values ('key1', 3, 2, 3);").get();

            return func(e);
        }, db_cfg_ptr).get();
    });
}

future<> take_snapshot(cql_test_env& e) {
    return e.db().invoke_on_all([] (database& db) {
        auto& cf = db.find_column_family("ks", "cf");
        return cf.snapshot(db, "test");
    });
}

SEASTAR_TEST_CASE(snapshot_works) {
    return do_with_some_data([] (cql_test_env& e) {
        take_snapshot(e).get();

        std::set<sstring> expected = {
            "manifest.json",
        };

        auto& cf = e.local_db().find_column_family("ks", "cf");
        lister::scan_dir(fs::path(cf.dir()), { directory_entry_type::regular }, [&expected] (fs::path parent_dir, directory_entry de) {
            expected.insert(de.name);
            return make_ready_future<>();
        }).get();
        // snapshot triggered a flush and wrote the data down.
        BOOST_REQUIRE_GT(expected.size(), 1);

        // all files were copied and manifest was generated
        lister::scan_dir((fs::path(cf.dir()) / "snapshots" / "test"), { directory_entry_type::regular }, [&expected] (fs::path parent_dir, directory_entry de) {
            expected.erase(de.name);
            return make_ready_future<>();
        }).get();

        BOOST_REQUIRE_EQUAL(expected.size(), 0);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(snapshot_list_okay) {
    return do_with_some_data([] (cql_test_env& e) {
        auto& cf = e.local_db().find_column_family("ks", "cf");
        take_snapshot(e).get();

        auto details = cf.get_snapshot_details().get0();
        BOOST_REQUIRE_EQUAL(details.size(), 1);

        auto sd = details["test"];
        BOOST_REQUIRE_EQUAL(sd.live, 0);
        BOOST_REQUIRE_GT(sd.total, 0);

        lister::scan_dir(fs::path(cf.dir()), { directory_entry_type::regular }, [] (fs::path parent_dir, directory_entry de) {
            fs::remove(parent_dir / de.name);
            return make_ready_future<>();
        }).get();

        auto sd_post_deletion = cf.get_snapshot_details().get0().at("test");

        BOOST_REQUIRE_EQUAL(sd_post_deletion.total, sd_post_deletion.live);
        BOOST_REQUIRE_EQUAL(sd.total, sd_post_deletion.live);

        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(snapshot_list_inexistent) {
    return do_with_some_data([] (cql_test_env& e) {
        auto& cf = e.local_db().find_column_family("ks", "cf");
        auto details = cf.get_snapshot_details().get0();
        BOOST_REQUIRE_EQUAL(details.size(), 0);
        return make_ready_future<>();
    });
}


SEASTAR_TEST_CASE(clear_snapshot) {
    return do_with_some_data([] (cql_test_env& e) {
        take_snapshot(e).get();
        auto& cf = e.local_db().find_column_family("ks", "cf");

        unsigned count = 0;
        lister::scan_dir((fs::path(cf.dir()) / "snapshots" / "test"), { directory_entry_type::regular }, [&count] (fs::path parent_dir, directory_entry de) {
            count++;
            return make_ready_future<>();
        }).get();
        BOOST_REQUIRE_GT(count, 1); // expect more than the manifest alone

        e.local_db().clear_snapshot("test", {"ks"}, "").get();
        count = 0;

        BOOST_REQUIRE_EQUAL(fs::exists(fs::path(cf.dir()) / "snapshots" / "test"), false);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(clear_nonexistent_snapshot) {
    // no crashes, no exceptions
    return do_with_some_data([] (cql_test_env& e) {
        e.local_db().clear_snapshot("test", {"ks"}, "").get();
        return make_ready_future<>();
    });
}


// toppartitions_query caused a lw_shared_ptr to cross shards when moving results, #5104
SEASTAR_TEST_CASE(toppartitions_cross_shard_schema_ptr) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("CREATE TABLE ks.tab (id int PRIMARY KEY)").get();
        db::toppartitions_query tq(e.db(), "ks", "tab", 1s, 100, 100);
        tq.scatter().get();
        auto q = e.prepare("INSERT INTO ks.tab(id) VALUES(?)").get0();
        // Generate many values to ensure crossing shards
        for (auto i = 0; i != 100; ++i) {
            e.execute_prepared(q, {cql3::raw_value::make_value(int32_type->decompose(i))}).get();
        }
        // This should trigger the bug in debug mode
        tq.gather().get();
    });
}

SEASTAR_THREAD_TEST_CASE(read_max_size) {
    do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("CREATE TABLE test (pk text, ck int, v text, PRIMARY KEY (pk, ck));").get();
        auto id = e.prepare("INSERT INTO test (pk, ck, v) VALUES (?, ?, ?);").get0();

        auto& db = e.local_db();
        auto& tab = db.find_column_family("ks", "test");
        auto s = tab.schema();

        auto pk = make_local_key(s);
        const auto raw_pk = utf8_type->decompose(data_value(pk));
        const auto cql3_pk = cql3::raw_value::make_value(raw_pk);

        const auto value = sstring(1024, 'a');
        const auto raw_value = utf8_type->decompose(data_value(value));
        const auto cql3_value = cql3::raw_value::make_value(raw_value);

        const int num_rows = 1024;

        for (int i = 0; i != num_rows; ++i) {
            const auto cql3_ck = cql3::raw_value::make_value(int32_type->decompose(data_value(i)));
            e.execute_prepared(id, {cql3_pk, cql3_ck, cql3_value}).get();
        }

        const auto partition_ranges = std::vector<dht::partition_range>{query::full_partition_range};

        const std::vector<std::pair<sstring, std::function<future<size_t>(schema_ptr, const query::read_command&)>>> query_methods{
                {"query_mutations()", [&db, &partition_ranges] (schema_ptr s, const query::read_command& cmd) -> future<size_t> {
                    return db.query_mutations(s, cmd, partition_ranges.front(), {}, db::no_timeout).then(
                            [] (const std::tuple<reconcilable_result, cache_temperature>& res) {
                        return std::get<0>(res).memory_usage();
                    });
                }},
                {"query()", [&db, &partition_ranges] (schema_ptr s, const query::read_command& cmd) -> future<size_t> {
                    return db.query(s, cmd, query::result_options::only_result(), partition_ranges, {}, db::no_timeout).then(
                            [] (const std::tuple<lw_shared_ptr<query::result>, cache_temperature>& res) {
                        return size_t(std::get<0>(res)->buf().size());
                    });
                }},
                {"query_mutations_on_all_shards()", [&e, &partition_ranges] (schema_ptr s, const query::read_command& cmd) -> future<size_t> {
                    return query_mutations_on_all_shards(e.db(), s, cmd, partition_ranges, {}, db::no_timeout).then(
                            [] (const std::tuple<foreign_ptr<lw_shared_ptr<reconcilable_result>>, cache_temperature>& res) {
                        return std::get<0>(res)->memory_usage();
                    });
                }}
        };

        for (auto [query_method_name, query_method] : query_methods) {
            for (auto allow_short_read : {true, false}) {
                for (auto max_size : {1024u, 1024u * 1024u, 1024u * 1024u * 1024u}) {
                    const auto should_throw = max_size < (num_rows * value.size() * 2) && !allow_short_read;
                    testlog.info("checking: query_method={}, allow_short_read={}, max_size={}, should_throw={}", query_method_name, allow_short_read, max_size, should_throw);
                    auto slice = s->full_slice();
                    if (allow_short_read) {
                        slice.options.set<query::partition_slice::option::allow_short_read>();
                    } else {
                        slice.options.remove<query::partition_slice::option::allow_short_read>();
                    }
                    query::read_command cmd(s->id(), s->version(), slice, query::max_result_size(max_size));
                    try {
                        auto size = query_method(s, cmd).get0();
                        // Just to ensure we are not interpreting empty results as success.
                        BOOST_REQUIRE(size != 0);
                        if (should_throw) {
                            BOOST_FAIL("Expected exception, but none was thrown.");
                        } else {
                            testlog.trace("No exception thrown, as expected.");
                        }
                    } catch (std::runtime_error& e) {
                        if (should_throw) {
                            testlog.trace("Exception thrown, as expected: {}", e);
                        } else {
                            BOOST_FAIL(fmt::format("Expected no exception, but caught: {}", e));
                        }
                    }
                }
            }
        }
    }).get();
}
