/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 *
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

#pragma once

#include "gms/i_endpoint_state_change_subscriber.hh"
#include "service/endpoint_lifecycle_subscriber.hh"
#include "locator/token_metadata.hh"
#include "gms/gossiper.hh"
#include <seastar/core/distributed.hh>
#include "dht/i_partitioner.hh"
#include "dht/token_range_endpoints.hh"
#include <seastar/core/sleep.hh>
#include "gms/application_state.hh"
#include <seastar/core/semaphore.hh>
#include <seastar/core/gate.hh>
#include "utils/fb_utilities.hh"
#include "utils/serialized_action.hh"
#include "database_fwd.hh"
#include "db/schema_features.hh"
#include "streaming/stream_state.hh"
#include <seastar/core/distributed.hh>
#include "utils/disk-error-handler.hh"
#include "service/migration_listener.hh"
#include "gms/feature_service.hh"
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/rwlock.hh>
#include "sstables/version.hh"
#include "cdc/metadata.hh"
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/lowres_clock.hh>

class node_ops_cmd_request;
class node_ops_cmd_response;
class node_ops_info;

namespace cql_transport { class controller; }

namespace db {
class system_distributed_keyspace;
namespace view {
class view_update_generator;
}
}

namespace dht {
class boot_strapper;
}

namespace gms {
class feature_service;
class gossiper;
};

namespace service {

class storage_service;

extern distributed<storage_service> _the_storage_service;
inline distributed<storage_service>& get_storage_service() {
    return _the_storage_service;
}
inline storage_service& get_local_storage_service() {
    return _the_storage_service.local();
}

enum class disk_error { regular, commit };

struct bind_messaging_port_tag {};
using bind_messaging_port = bool_class<bind_messaging_port_tag>;

struct storage_service_config {
    size_t available_memory;
};

class node_ops_meta_data {
    utils::UUID _ops_uuid;
    gms::inet_address _coordinator;
    std::function<future<> ()> _abort;
    std::function<void ()> _signal;
    shared_ptr<node_ops_info> _ops;
    seastar::timer<lowres_clock> _watchdog;
    std::chrono::seconds _watchdog_interval{30};
    bool _aborted = false;
public:
    explicit node_ops_meta_data(
            utils::UUID ops_uuid,
            gms::inet_address coordinator,
            shared_ptr<node_ops_info> ops,
            std::function<future<> ()> abort_func,
            std::function<void ()> signal_func);
    shared_ptr<node_ops_info> get_ops_info();
    future<> abort();
    void update_watchdog();
    void cancel_watchdog();
};

/**
 * This abstraction contains the token/identifier of this node
 * on the identifier space. This token gets gossiped around.
 * This class will also maintain histograms of the load information
 * of other nodes in the cluster.
 */
class storage_service : public service::migration_listener, public gms::i_endpoint_state_change_subscriber,
        public seastar::async_sharded_service<storage_service>, public seastar::peering_sharded_service<storage_service> {
private:
    using token = dht::token;
    using token_range_endpoints = dht::token_range_endpoints;
    using endpoint_details = dht::endpoint_details;
    using boot_strapper = dht::boot_strapper;
    using token_metadata = locator::token_metadata;
    using shared_token_metadata = locator::shared_token_metadata;
    using token_metadata_ptr = locator::token_metadata_ptr;
    using mutable_token_metadata_ptr = locator::mutable_token_metadata_ptr;
    using token_metadata_lock = locator::token_metadata_lock;
    using application_state = gms::application_state;
    using inet_address = gms::inet_address;
    using versioned_value = gms::versioned_value;

    abort_source& _abort_source;
    gms::feature_service& _feature_service;
    distributed<database>& _db;
    gms::gossiper& _gossiper;
    sharded<service::migration_notifier>& _mnotifier;
    sharded<netw::messaging_service>& _messaging;
    // Note that this is obviously only valid for the current shard. Users of
    // this facility should elect a shard to be the coordinator based on any
    // given objective criteria
    //
    // It shouldn't be impossible to actively serialize two callers if the need
    // ever arise.
    bool _loading_new_sstables = false;
    friend class cql_transport::controller;
    sstring _operation_in_progress;
    bool _force_remove_completion = false;
    bool _ms_stopped = false;
    bool _stream_manager_stopped = false;
    seastar::metrics::metric_groups _metrics;
    size_t _service_memory_total;
    semaphore _service_memory_limiter;
    using client_shutdown_hook = noncopyable_function<void()>;
    std::vector<std::pair<std::string, client_shutdown_hook>> _client_shutdown_hooks;
    std::vector<std::any> _listeners;

    /* For unit tests only.
     *
     * Currently used when choosing the timestamp of the first CDC stream generation:
     * normally we choose a timestamp in the future so other nodes have a chance to learn about it
     * before it starts operating, but in the single-node-cluster case this is not necessary
     * and would only slow down tests (by having them wait).
     */
    bool _for_testing;

    std::unordered_map<utils::UUID, node_ops_meta_data> _node_ops;
    std::list<std::optional<utils::UUID>> _node_ops_abort_queue;
    seastar::condition_variable _node_ops_abort_cond;
    named_semaphore _node_ops_abort_sem{1, named_semaphore_exception_factory{"node_ops_abort_sem"}};
    future<> _node_ops_abort_thread;
    void node_ops_update_heartbeat(utils::UUID ops_uuid);
    void node_ops_done(utils::UUID ops_uuid);
    void node_ops_abort(utils::UUID ops_uuid);
    void node_ops_singal_abort(std::optional<utils::UUID> ops_uuid);
    future<> node_ops_abort_thread();
public:
    storage_service(abort_source& as, distributed<database>& db, gms::gossiper& gossiper, sharded<db::system_distributed_keyspace>&, sharded<db::view::view_update_generator>&, gms::feature_service& feature_service, storage_service_config config, sharded<service::migration_notifier>& mn, locator::shared_token_metadata& stm, sharded<netw::messaging_service>& ms, /* only for tests */ bool for_testing = false);

    // Needed by distributed<>
    future<> stop();
    void init_messaging_service();
    future<> uninit_messaging_service();

private:
    future<token_metadata_lock> get_token_metadata_lock() noexcept;
    future<> with_token_metadata_lock(std::function<future<> ()>) noexcept;

    // Acquire the token_metadata lock and get a mutable_token_metadata_ptr.
    // Pass that ptr to \c func, and when successfully done,
    // replicate it to all cores.
    // Note: must be called on shard 0.
    future<> mutate_token_metadata(std::function<future<> (mutable_token_metadata_ptr)> func) noexcept;

    // Update pending ranges locally and then replicate to all cores.
    // Should be serialized under token_metadata_lock.
    // Must be called on shard 0.
    future<> update_pending_ranges(mutable_token_metadata_ptr tmptr, sstring reason);
    future<> update_pending_ranges(sstring reason);
    future<> keyspace_changed(const sstring& ks_name);
    void register_metrics();
    future<> publish_schema_version();
    void install_schema_version_change_listener();

    future<mutable_token_metadata_ptr> get_mutable_token_metadata_ptr() noexcept {
        return _shared_token_metadata.get()->clone_async().then([] (token_metadata tm) {
            return make_ready_future<mutable_token_metadata_ptr>(make_token_metadata_ptr(std::move(tm)));
        });
    }
public:
    static future<> update_topology(inet_address endpoint);

    token_metadata_ptr get_token_metadata_ptr() const noexcept {
        return _shared_token_metadata.get();
    }

    const locator::token_metadata& get_token_metadata() const noexcept {
        return *_shared_token_metadata.get();
    }

    cdc::metadata& get_cdc_metadata() {
        return _cdc_metadata;
    }

    const service::migration_notifier& get_migration_notifier() const {
        return _mnotifier.local();
    }

    service::migration_notifier& get_migration_notifier() {
        return _mnotifier.local();
    }

    future<> gossip_snitch_info();
    future<> gossip_sharder();

    distributed<database>& db() {
        return _db;
    }

    gms::feature_service& features() { return _feature_service; }
    const gms::feature_service& features() const { return _feature_service; }

    semaphore& service_memory_limiter() {
        return _service_memory_limiter;
    }

private:
    bool is_auto_bootstrap() const;
    inet_address get_broadcast_address() const {
        return utils::fb_utilities::get_broadcast_address();
    }
    /* This abstraction maintains the token/endpoint metadata information */
    mutable_token_metadata_ptr _pending_token_metadata_ptr;
    shared_token_metadata& _shared_token_metadata;

    // Maintains the set of known CDC generations used to pick streams for log writes (i.e., the partition keys of these log writes).
    // Updated in response to certain gossip events (see the handle_cdc_generation function).
    cdc::metadata _cdc_metadata;
public:
    std::chrono::milliseconds get_ring_delay();
private:

    std::unordered_set<inet_address> _replicating_nodes;

    std::optional<inet_address> _removing_node;

    /* Are we starting this node in bootstrap mode? */
    bool _is_bootstrap_mode = false;

    bool _initialized = false;

    bool _joined = false;

public:
    enum class mode { STARTING, NORMAL, JOINING, LEAVING, DECOMMISSIONED, MOVING, DRAINING, DRAINED };
private:
    mode _operation_mode = mode::STARTING;
    friend std::ostream& operator<<(std::ostream& os, const mode& mode);
    /* Used for tracking drain progress */
public:
    struct drain_progress {
        int32_t total_cfs;
        int32_t remaining_cfs;

        drain_progress& operator+=(const drain_progress& other) {
            total_cfs += other.total_cfs;
            remaining_cfs += other.remaining_cfs;
            return *this;
        }
    };
private:
    drain_progress _drain_progress{};


    std::vector<endpoint_lifecycle_subscriber*> _lifecycle_subscribers;

    std::unordered_set<token> _bootstrap_tokens;

    /* The timestamp of the CDC streams generation that this node has proposed when joining.
     * This value is nullopt only when:
     * 1. this node is being upgraded from a non-CDC version,
     * 2. this node is starting for the first time or restarting with CDC previously disabled,
     *    in which case the value should become populated before we leave the join_token_ring procedure.
     */
    std::optional<db_clock::time_point> _cdc_streams_ts;

public:
    void enable_all_features();

    /* Broadcasts the chosen tokens through gossip,
     * together with a CDC streams timestamp (if we start a new CDC generation) and STATUS=NORMAL. */
    void set_gossip_tokens(const std::unordered_set<dht::token>&, std::optional<db_clock::time_point>);

    void register_subscriber(endpoint_lifecycle_subscriber* subscriber);

    void unregister_subscriber(endpoint_lifecycle_subscriber* subscriber);

    // should only be called via JMX
    future<> stop_gossiping();

    // should only be called via JMX
    future<> start_gossiping(bind_messaging_port do_bind = bind_messaging_port::yes);

    // should only be called via JMX
    future<bool> is_gossip_running();

    void register_client_shutdown_hook(std::string name, client_shutdown_hook hook) {
        _client_shutdown_hooks.push_back({std::move(name), std::move(hook)});
    }
    void unregister_client_shutdown_hook(std::string name) {
        auto it = std::find_if(_client_shutdown_hooks.begin(), _client_shutdown_hooks.end(),
                [&name] (const std::pair<std::string, client_shutdown_hook>& hook) { return hook.first == name; });
        if (it != _client_shutdown_hooks.end()) {
            _client_shutdown_hooks.erase(it);
        }
    }
private:
    future<> do_stop_ms();
    future<> do_stop_stream_manager();
    // Runs in thread context
    void shutdown_client_servers();

    // Tokens and the CDC streams timestamp of the replaced node.
    using replacement_info = std::pair<std::unordered_set<token>, std::optional<db_clock::time_point>>;
    future<replacement_info> prepare_replacement_info(std::unordered_set<gms::inet_address> initial_contact_nodes,
            const std::unordered_map<gms::inet_address, sstring>& loaded_peer_features, bind_messaging_port do_bind = bind_messaging_port::yes);

public:
    future<bool> is_initialized();

    future<> check_for_endpoint_collision(std::unordered_set<gms::inet_address> initial_contact_nodes,
            const std::unordered_map<gms::inet_address, sstring>& loaded_peer_features, bind_messaging_port do_bind = bind_messaging_port::yes);

    /*!
     * \brief Init the messaging service part of the service.
     *
     * This is the first part of the initialization, call this method
     * first.
     *
     * After this method is completed, it is ok to start the storage_service
     * API.
     * \see init_server_without_the_messaging_service_part
     */
    future<> init_messaging_service_part();
    /*!
     * \brief Uninit the messaging service part of the service.
     */
    future<> uninit_messaging_service_part();

    /*!
     * \brief complete the server initialization
     *
     * The storage_service initialization is done in two parts.
     *
     * you first call init_messaging_service_part and then
     * you call init_server_without_the_messaging_service_part.
     *
     * It is safe to start the API after init_messaging_service_part
     * completed
     *
     * Must be called on shard 0.
     *
     * \see init_messaging_service_part
     */
    future<> init_server(bind_messaging_port do_bind = bind_messaging_port::yes);

    future<> join_cluster();

    future<> drain_on_shutdown();

    future<> stop_transport();

    void flush_column_families();

private:
    bool should_bootstrap();
    bool is_first_node();
    void prepare_to_join(
            std::unordered_set<gms::inet_address> initial_contact_nodes,
            std::unordered_set<gms::inet_address> loaded_endpoints,
            std::unordered_map<gms::inet_address, sstring> loaded_peer_features,
            bind_messaging_port do_bind = bind_messaging_port::yes);
    void join_token_ring(int delay);
    void maybe_start_sys_dist_ks();
public:
    inline bool is_joined() const {
        // Every time we set _joined, we do it on all shards, so we can read its
        // value locally.
        return _joined;
    }

    future<> rebuild(sstring source_dc);

private:
    void set_mode(mode m, bool log);
    void set_mode(mode m, sstring msg, bool log);
    void mark_existing_views_as_built();

    // Stream data for which we become a new replica.
    // Before that, if we're not replacing another node, inform other nodes about our chosen tokens (_bootstrap_tokens)
    // and wait for RING_DELAY ms so that we receive new writes from coordinators during streaming.
    void bootstrap();

public:
    bool is_bootstrap_mode() const {
        return _is_bootstrap_mode;
    }

    /**
     * Return the rpc address associated with an endpoint as a string.
     * @param endpoint The endpoint to get rpc address for
     * @return the rpc address
     */
    sstring get_rpc_address(const inet_address& endpoint) const;

    std::unordered_map<dht::token_range, std::vector<inet_address>> get_range_to_address_map(const sstring& keyspace) const;

    std::unordered_map<dht::token_range, std::vector<inet_address>> get_range_to_address_map_in_local_dc(
            const sstring& keyspace) const;

    std::vector<token> get_tokens_in_local_dc() const;

    bool is_local_dc(const inet_address& targetHost) const;

    std::unordered_map<dht::token_range, std::vector<inet_address>> get_range_to_address_map(const sstring& keyspace,
            const std::vector<token>& sorted_tokens) const;

    /**
     * The same as {@code describeRing(String)} but converts TokenRange to the String for JMX compatibility
     *
     * @param keyspace The keyspace to fetch information about
     *
     * @return a List of TokenRange(s) converted to String for the given keyspace
     */

    /*
     * describeRingJMX will be implemented in the API
     * It is left here just as a marker that there is no need to implement it
     * here
     */
    //std::vector<sstring> describeRingJMX(const sstring& keyspace) const {

    std::vector<token_range_endpoints> describe_ring(const sstring& keyspace, bool include_only_local_dc = false) const;

    /**
     * Retrieve a map of tokens to endpoints, including the bootstrapping ones.
     *
     * @return a map of tokens to endpoints in ascending order
     */
    std::map<token, inet_address> get_token_to_endpoint_map();

    /**
     * Construct the range to endpoint mapping based on the true view
     * of the world.
     * @param ranges
     * @return mapping of ranges to the replicas responsible for them.
    */
    std::unordered_map<dht::token_range, std::vector<inet_address>> construct_range_to_endpoint_map(
            const sstring& keyspace,
            const dht::token_range_vector& ranges) const;
public:
    virtual void on_join(gms::inet_address endpoint, gms::endpoint_state ep_state) override;
    virtual void before_change(gms::inet_address endpoint, gms::endpoint_state current_state, gms::application_state new_state_key, const gms::versioned_value& new_value) override;
    /*
     * Handle the reception of a new particular ApplicationState for a particular endpoint. Note that the value of the
     * ApplicationState has not necessarily "changed" since the last known value, if we already received the same update
     * from somewhere else.
     *
     * onChange only ever sees one ApplicationState piece change at a time (even if many ApplicationState updates were
     * received at the same time), so we perform a kind of state machine here. We are concerned with two events: knowing
     * the token associated with an endpoint, and knowing its operation mode. Nodes can start in either bootstrap or
     * normal mode, and from bootstrap mode can change mode to normal. A node in bootstrap mode needs to have
     * pendingranges set in TokenMetadata; a node in normal mode should instead be part of the token ring.
     *
     * Normal progression of ApplicationState.STATUS values for a node should be like this:
     * STATUS_BOOTSTRAPPING,token
     *   if bootstrapping. stays this way until all files are received.
     * STATUS_NORMAL,token
     *   ready to serve reads and writes.
     * STATUS_LEAVING,token
     *   get ready to leave the cluster as part of a decommission
     * STATUS_LEFT,token
     *   set after decommission is completed.
     *
     * Other STATUS values that may be seen (possibly anywhere in the normal progression):
     * STATUS_MOVING,newtoken
     *   set if node is currently moving to a new token in the ring
     * REMOVING_TOKEN,deadtoken
     *   set if the node is dead and is being removed by its REMOVAL_COORDINATOR
     * REMOVED_TOKEN,deadtoken
     *   set if the node is dead and has been removed by its REMOVAL_COORDINATOR
     *
     * Note: Any time a node state changes from STATUS_NORMAL, it will not be visible to new nodes. So it follows that
     * you should never bootstrap a new node during a removenode, decommission or move.
     */
    virtual void on_change(inet_address endpoint, application_state state, const versioned_value& value) override;
    virtual void on_alive(gms::inet_address endpoint, gms::endpoint_state state) override;
    virtual void on_dead(gms::inet_address endpoint, gms::endpoint_state state) override;
    virtual void on_remove(gms::inet_address endpoint) override;
    virtual void on_restart(gms::inet_address endpoint, gms::endpoint_state state) override;

public:
    // For migration_listener
    virtual void on_create_keyspace(const sstring& ks_name) override { keyspace_changed(ks_name).get(); }
    virtual void on_create_column_family(const sstring& ks_name, const sstring& cf_name) override {}
    virtual void on_create_user_type(const sstring& ks_name, const sstring& type_name) override {}
    virtual void on_create_function(const sstring& ks_name, const sstring& function_name) override {}
    virtual void on_create_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    virtual void on_create_view(const sstring& ks_name, const sstring& view_name) override {}

    virtual void on_update_keyspace(const sstring& ks_name) override { keyspace_changed(ks_name).get(); }
    virtual void on_update_column_family(const sstring& ks_name, const sstring& cf_name, bool) override {}
    virtual void on_update_user_type(const sstring& ks_name, const sstring& type_name) override {}
    virtual void on_update_function(const sstring& ks_name, const sstring& function_name) override {}
    virtual void on_update_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    virtual void on_update_view(const sstring& ks_name, const sstring& view_name, bool columns_changed) override {}

    virtual void on_drop_keyspace(const sstring& ks_name) override { keyspace_changed(ks_name).get(); }
    virtual void on_drop_column_family(const sstring& ks_name, const sstring& cf_name) override {}
    virtual void on_drop_user_type(const sstring& ks_name, const sstring& type_name) override {}
    virtual void on_drop_function(const sstring& ks_name, const sstring& function_name) override {}
    virtual void on_drop_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    virtual void on_drop_view(const sstring& ks_name, const sstring& view_name) override {}
private:
    void update_peer_info(inet_address endpoint);
    void do_update_system_peers_table(gms::inet_address endpoint, const application_state& state, const versioned_value& value);

    std::unordered_set<token> get_tokens_for(inet_address endpoint);

    /* Retrieve the CDC generation which starts at the given timestamp (from a distributed table created for this purpose)
     * and start using it for CDC log writes if it's not obsolete.
     */
    void handle_cdc_generation(std::optional<db_clock::time_point>);
    /* Returns `true` iff we started using the generation (it was not obsolete),
     * which means that this node might write some CDC log entries using streams from this generation. */
    bool do_handle_cdc_generation(db_clock::time_point);
    /* Wrapper around `do_handle_cdc_generation` which intercepts timeout/unavailability exceptions.
     * Returns: do_handle_cdc_generation(ts). */
    bool do_handle_cdc_generation_intercept_nonfatal_errors(db_clock::time_point ts);

    /* If `handle_cdc_generation` fails, it schedules an asynchronous retry in the background
     * using `async_handle_cdc_generation`.
     */
    void async_handle_cdc_generation(db_clock::time_point);

    /* Scan CDC generation timestamps gossiped by other nodes and retrieve the latest one.
     * This function should be called once at the end of the node startup procedure
     * (after the node is started and running normally, it will retrieve generations on gossip events instead).
     */
    void scan_cdc_generations();

public:
    future<> check_and_repair_cdc_streams();
private:
    // Should be serialized under token_metadata_lock.
    future<> replicate_to_all_cores(mutable_token_metadata_ptr tmptr) noexcept;
    sharded<db::system_distributed_keyspace>& _sys_dist_ks;
    sharded<db::view::view_update_generator>& _view_update_generator;
    serialized_action _schema_version_publisher;
private:
    /**
     * Handle node bootstrap
     *
     * @param endpoint bootstrapping node
     */
    void handle_state_bootstrap(inet_address endpoint);

    /**
     * Handle node move to normal state. That is, node is entering token ring and participating
     * in reads.
     *
     * @param endpoint node
     */
    void handle_state_normal(inet_address endpoint);

    /**
     * Handle node preparing to leave the ring
     *
     * @param endpoint node
     */
    void handle_state_leaving(inet_address endpoint);

    /**
     * Handle node leaving the ring. This will happen when a node is decommissioned
     *
     * @param endpoint If reason for leaving is decommission, endpoint is the leaving node.
     * @param pieces STATE_LEFT,token
     */
    void handle_state_left(inet_address endpoint, std::vector<sstring> pieces);

    /**
     * Handle node moving inside the ring.
     *
     * @param endpoint moving endpoint address
     * @param pieces STATE_MOVING, token
     */
    void handle_state_moving(inet_address endpoint, std::vector<sstring> pieces);

    /**
     * Handle notification that a node being actively removed from the ring via 'removenode'
     *
     * @param endpoint node
     * @param pieces either REMOVED_TOKEN (node is gone) or REMOVING_TOKEN (replicas need to be restored)
     */
    void handle_state_removing(inet_address endpoint, std::vector<sstring> pieces);

    /**
     * Handle notification that a node is replacing another node.
     *
     * @param endpoint node
     */
    void handle_state_replacing(inet_address endpoint);

private:
    void excise(std::unordered_set<token> tokens, inet_address endpoint);
    void excise(std::unordered_set<token> tokens, inet_address endpoint, long expire_time);

    /** unlike excise we just need this endpoint gone without going through any notifications **/
    void remove_endpoint(inet_address endpoint);

    void add_expire_time_if_found(inet_address endpoint, int64_t expire_time);

    int64_t extract_expire_time(const std::vector<sstring>& pieces) const {
        return std::stoll(pieces[2]);
    }

    /**
     * Finds living endpoints responsible for the given ranges
     *
     * @param keyspaceName the keyspace ranges belong to
     * @param ranges the ranges to find sources for
     * @param tm the token metadata
     * @return multimap of addresses to ranges the address is responsible for
     *
     * @note The function must be called from a seastar thread.
     *       The caller is responsible for keeping @ref tm valid across the call.
     */
    std::unordered_multimap<inet_address, dht::token_range> get_new_source_ranges(const sstring& keyspaceName, const dht::token_range_vector& ranges, const token_metadata& tm);
public:
    future<> confirm_replication(inet_address node);

private:

    /**
     * Sends a notification to a node indicating we have finished replicating data.
     *
     * @param remote node to send notification to
     */
    future<> send_replication_notification(inet_address remote);

    /**
     * Called when an endpoint is removed from the ring. This function checks
     * whether this node becomes responsible for new ranges as a
     * consequence and streams data if needed.
     *
     * This is rather ineffective, but it does not matter so much
     * since this is called very seldom
     *
     * @param endpoint the node that left
     */
    future<> restore_replica_count(inet_address endpoint, inet_address notify_endpoint);

    // needs to be modified to accept either a keyspace or ARS.
    std::unordered_multimap<dht::token_range, inet_address> get_changed_ranges_for_leaving(sstring keyspace_name, inet_address endpoint);

public:

    sstring get_release_version();

    sstring get_schema_version();

    future<std::unordered_map<sstring, std::vector<sstring>>> describe_schema_versions();


    /**
     * Get all ranges an endpoint is responsible for (by keyspace)
     * Replication strategy's get_ranges() guarantees that no wrap-around range is returned.
     * @param ep endpoint we are interested in.
     * @return ranges for the specified endpoint.
     */
    dht::token_range_vector get_ranges_for_endpoint(const sstring& name, const gms::inet_address& ep) const;

    /**
     * Get all ranges that span the ring given a set
     * of tokens. All ranges are in sorted order of
     * ranges.
     * @return ranges in sorted order
    */
    dht::token_range_vector get_all_ranges(const std::vector<token>& sorted_tokens) const;
    /**
     * This method returns the N endpoints that are responsible for storing the
     * specified key i.e for replication.
     *
     * @param keyspaceName keyspace name also known as keyspace
     * @param cf Column family name
     * @param key key for which we need to find the endpoint
     * @return the endpoint responsible for this key
     */
    std::vector<gms::inet_address> get_natural_endpoints(const sstring& keyspace,
            const sstring& cf, const sstring& key) const;

    /**
     * This method returns the N endpoints that are responsible for storing the
     * specified key i.e for replication.
     *
     * @param keyspaceName keyspace name also known as keyspace
     * @param pos position for which we need to find the endpoint
     * @return the endpoint responsible for this token
     */
    std::vector<gms::inet_address>  get_natural_endpoints(const sstring& keyspace, const token& pos) const;

    /**
     * @return Vector of Token ranges (_not_ keys!) together with estimated key count,
     *      breaking up the data this node is responsible for into pieces of roughly keys_per_split
     */
    std::vector<std::pair<dht::token_range, uint64_t>> get_splits(const sstring& ks_name,
            const sstring& cf_name,
            range<dht::token> range,
            uint32_t keys_per_split);
public:
    future<> decommission();

private:
    /**
     * Broadcast leaving status and update local _token_metadata accordingly
     */
    future<> start_leaving();
    void leave_ring();
    void unbootstrap();

public:
    future<> move(sstring new_token) {
        // FIXME: getPartitioner().getTokenFactory().validate(newToken);
        return move(dht::token::from_sstring(new_token));
    }

private:
    /**
     * move the node to new token or find a new token to boot to according to load
     *
     * @param newToken new token to boot to, or if null, find balanced token to boot to
     *
     * @throws IOException on any I/O operation error
     */
    future<> move(token new_token);
public:

    /**
     * Get the status of a token removal.
     */
    future<sstring> get_removal_status();

    /**
     * Force a remove operation to complete. This may be necessary if a remove operation
     * blocks forever due to node/stream failure. removeToken() must be called
     * first, this is a last resort measure.  No further attempt will be made to restore replicas.
     */
    future<> force_remove_completion();

public:
    /**
     * Remove a node that has died, attempting to restore the replica count.
     * If the node is alive, decommission should be attempted.  If decommission
     * fails, then removeToken should be called.  If we fail while trying to
     * restore the replica count, finally forceRemoveCompleteion should be
     * called to forcibly remove the node without regard to replica count.
     *
     * @param hostIdString token for the node
     */
    future<> removenode(sstring host_id_string, std::list<gms::inet_address> ignore_nodes);
    future<node_ops_cmd_response> node_ops_cmd_handler(gms::inet_address coordinator, node_ops_cmd_request req);

    future<sstring> get_operation_mode();

    future<bool> is_starting();

    drain_progress get_drain_progress() const {
        return _drain_progress;
    }

    /**
     * Shuts node off to writes, empties memtables and the commit log.
     * There are two differences between drain and the normal shutdown hook:
     * - Drain waits for in-progress streaming to complete
     * - Drain flushes *all* columnfamilies (shutdown hook only flushes non-durable CFs)
     */
    future<> drain();

    future<std::map<gms::inet_address, float>> get_ownership();

    future<std::map<gms::inet_address, float>> effective_ownership(sstring keyspace_name);

    future<std::unordered_map<sstring, sstring>> view_build_statuses(sstring keyspace, sstring view_name) const;

private:
    /**
     * Seed data to the endpoints that will be responsible for it at the future
     *
     * @param rangesToStreamByKeyspace keyspaces and data ranges with endpoints included for each
     * @return async Future for whether stream was success
     */
    future<> stream_ranges(std::unordered_map<sstring, std::unordered_multimap<dht::token_range, inet_address>> ranges_to_stream_by_keyspace);

public:
    int32_t get_exception_count();

    /**
     * Load new SSTables not currently tracked by the system
     *
     * This can be called, for instance, after copying a batch of SSTables to a CF directory.
     *
     * This should not be called in parallel for the same keyspace / column family, and doing
     * so will throw an std::runtime_exception.
     *
     * @param ks_name the keyspace in which to search for new SSTables.
     * @param cf_name the column family in which to search for new SSTables.
     * @return a future<> when the operation finishes.
     */
    future<> load_new_sstables(sstring ks_name, sstring cf_name);

    future<> set_tables_autocompaction(const sstring &keyspace, std::vector<sstring> tables, bool enabled);

    template <typename Func>
    auto run_with_api_lock(sstring operation, Func&& func) {
        return get_storage_service().invoke_on(0, [operation = std::move(operation),
                func = std::forward<Func>(func)] (storage_service& ss) mutable {
            if (!ss._operation_in_progress.empty()) {
                throw std::runtime_error(format("Operation {} is in progress, try again", ss._operation_in_progress));
            }
            ss._operation_in_progress = std::move(operation);
            return func(ss).finally([&ss] {
                ss._operation_in_progress = sstring();
            });
        });
    }

    template <typename Func>
    auto run_with_no_api_lock(Func&& func) {
        return get_storage_service().invoke_on(0, [func = std::forward<Func>(func)] (storage_service& ss) mutable {
            return func(ss);
        });
    }
private:
    void do_isolate_on_error(disk_error type);
    future<> isolate();

    void notify_down(inet_address endpoint);
    void notify_left(inet_address endpoint);
    void notify_up(inet_address endpoint);
    void notify_joined(inet_address endpoint);
    void notify_cql_change(inet_address endpoint, bool ready);
public:
    future<bool> is_cleanup_allowed(sstring keyspace);
    bool is_repair_based_node_ops_enabled();
};

future<> init_storage_service(sharded<abort_source>& abort_sources, distributed<database>& db, sharded<gms::gossiper>& gossiper,
        sharded<db::system_distributed_keyspace>& sys_dist_ks,
        sharded<db::view::view_update_generator>& view_update_generator, sharded<gms::feature_service>& feature_service,
        storage_service_config config, sharded<service::migration_notifier>& mn, sharded<locator::shared_token_metadata>& stm,
        sharded<netw::messaging_service>& ms);
future<> deinit_storage_service();

}
