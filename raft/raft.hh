/*
 * Copyright (C) 2020 ScyllaDB
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

#include <vector>
#include <functional>
#include <boost/container/deque.hpp>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/future.hh>
#include <seastar/util/log.hh>
#include "bytes_ostream.hh"
#include "utils/UUID.hh"
#include "internal.hh"

namespace raft {
// Keeps user defined command. A user is responsible to serialize
// a state machine operation into it before passing to raft and
// deserialize in apply() before applying.
using command = bytes_ostream;
using command_cref = std::reference_wrapper<const command>;

extern seastar::logger logger;

// This is user provided id for a snapshot
using snapshot_id = internal::tagged_id<struct shapshot_id_tag>;
// Unique identifier of a server in a Raft group
using server_id = internal::tagged_id<struct server_id_tag>;

// This type represents the raft term
using term_t = internal::tagged_uint64<struct term_tag>;
// This type represensts the index into the raft log
using index_t = internal::tagged_uint64<struct index_tag>;

using clock_type = lowres_clock;

// Opaque connection properties. May contain ip:port pair for instance.
// This value is disseminated between cluster member
// through regular log replication as part of a configuration
// log entry. Upon receiving it a server passes it down to
// RPC module through add_server() call where it is deserialized
// and used to obtain connection info for the node `id`. After a server
// is added to the RPC module RPC's send functions can be used to communicate
// with it using its `id`.
using server_info = bytes;

struct server_address {
    server_id id;
    server_info info;
};

struct configuration {
    std::vector<server_address> servers;

    configuration(std::initializer_list<server_id> ids) {
        servers.reserve(ids.size());
        for (auto&& id : ids) {
            servers.emplace_back(server_address{std::move(id)});
        }
    }
    configuration() = default;
};

struct log_entry {
    // Dummy entry is used when a leader needs to commit an entry
    // (after leadership change for instance) but there is nothing
    // else to commit.
    struct dummy {};
    term_t term;
    index_t idx;
    std::variant<command, configuration, dummy> data;
};

using log_entry_ptr = seastar::lw_shared_ptr<const log_entry>;

struct error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct not_a_leader : public error {
    server_id leader;
    explicit not_a_leader(server_id l) : error("Not a leader"), leader(l) {}
};

struct dropped_entry : public error {
    dropped_entry() : error("Entry was dropped because of a leader change") {}
};

struct commit_status_unknown : public error {
    commit_status_unknown() : error("Commit staus of the entry is unknown") {}
};

struct stopped_error : public error {
    stopped_error() : error("Raft instance is stopped") {}
};

struct conf_change_in_progress : public error {
    conf_change_in_progress() : error("A configuration change is already in progress") {}
};

struct config_error : public error {
    using error::error;
};

struct snapshot {
    // Index and term of last entry in the snapshot
    index_t idx = index_t(0);
    term_t term = term_t(0);
    // The committed configuration in the snapshot
    configuration config;
    // Id of the snapshot.
    snapshot_id id;
};

struct append_request_base {
    // The leader's term.
    term_t current_term;
    // So that follower can redirect clients
    // In practice we do not need it since we should know sender's id anyway.
    server_id leader_id;
    // Index of the log entry immediately preceding new ones
    index_t prev_log_idx;
    // Term of prev_log_idx entry.
    term_t prev_log_term;
    // The leader's commit_idx.
    index_t leader_commit_idx;
};

struct append_request_send : public append_request_base {
    // Log entries to store (empty vector for heartbeat; may send more
    // than one entry for efficiency).
    std::vector<log_entry_ptr> entries;
};
struct append_request_recv : public append_request_base {
    // Same as for append_request_send but unlike it here the
    // message owns the entries.
    std::vector<log_entry> entries;
};

struct append_reply {
    struct rejected {
        // Index of non matching entry that caused the request
        // to be rejected.
        index_t non_matching_idx;
        // Last index in the follower's log, can be used to find next
        // matching index more efficiently.
        index_t last_idx;
    };
    struct accepted {
        // Last entry that was appended (may be smaller than max log index
        // in case follower's log is longer and appended entries match).
        index_t last_new_idx;
    };
    // Current term, for leader to update itself.
    term_t current_term;
    // Contains an index of the last commited entry on the follower
    // It is used by a leader to know if a follower is behind and issuing
    // empty append entry with updates commit_idx if it is
    // Regular RAFT handles this by always sending enoty append requests 
    // as a hearbeat.
    index_t commit_idx;
    std::variant<rejected, accepted> result;
};

struct vote_request {
    // The candidate’s term.
    term_t current_term;
    // The index of the candidate's last log entry.
    index_t last_log_idx;
    // The term of the candidate's last log entry.
    term_t last_log_term;
};

struct vote_reply {
    // Current term, for the candidate to update itself.
    term_t current_term;
    // True means the candidate received a vote.
    bool vote_granted;
};

struct install_snapshot {
    // Current term on a leader
    term_t current_term;
    // A snapshot to install
    snapshot snp;
};

struct snapshot_reply {
    bool success;
};

using rpc_message = std::variant<append_request_send, append_reply, vote_request, vote_reply, install_snapshot, snapshot_reply>;

// we need something that can be truncated form both sides.
// std::deque move constructor is not nothrow hence cannot be used
using log_entries = boost::container::deque<log_entry_ptr>;

// rpc, storage and satte_machine classes will have to be implemented by the
// raft user to provide network, persistency and busyness logic support
// repectively.
class rpc;
class storage;

// Any of the functions may return an error, but it will kill the
// raft instance that uses it. Depending on what state the failure
// leaves the state is the raft instance will either have to be recreated
// with the same state machine and rejoined the cluster with the same server_id
// or it new raft instance will have to be created with empty state machine and
// it will have to rejoin to the cluster with different server_id through
// configuration change.
class state_machine {
public:
    virtual ~state_machine() {}

    // This is called after entries are committed (replicated to
    // at least quorum of servers). If a provided vector contains
    // more than one entry all of them will be committed simultaneously.
    // Will be eventually called on all replicas, for all commited commands.
    // Raft owns the data since it may be still replicating.
    // Raft will not call another apply until the retuned future
    // will not become ready.
    virtual future<> apply(std::vector<command_cref> command) = 0;

    // The function suppose to take a snapshot of a state machine
    // To be called during log compaction or when a leader brings
    // a lagging follower up-to-date
    virtual future<snapshot_id> take_snapshot() = 0;

    // The function drops a snapshot with a provided id
    virtual void drop_snapshot(snapshot_id id) = 0;

    // reload state machine from a snapshot id
    // To be used by a restarting server or by a follower that
    // catches up to a leader
    virtual future<> load_snapshot(snapshot_id id) = 0;

    // stops the state machine instance by aborting the work
    // that can be aborted and waiting for all the rest to complete
    // any unfinished apply/snapshot operation may return an error after
    // this function is called
    virtual future<> abort() = 0;
};

class rpc_server;

// It is safe for for rpc implementation to drop any message.
// Error returned by send function will be ignored. All send_()
// functions can be called concurrently, returned future should be
// waited only for back pressure purposes (unless specified otherwise in
// the function's comment). Values passed by reference may be freed as soon
// as function returns.
class rpc {
protected:
    // Pointer to Raft server. Needed for passing RPC messages.
    rpc_server* _client = nullptr;
public:
    virtual ~rpc() {}

    // Send a snapshot snap to a server server_id.
    // A returned future is resolved when snapshot is sent and
    // successfully applied by a receiver. Will be waited to
    // know if a snapshot transfer succeeded.
    virtual future<> send_snapshot(server_id server_id, const install_snapshot& snap) = 0;

    // Send provided append_request to the supplied server, does
    // not wait for reply. The returned future resolves when
    // message is sent. It does not mean it was received.
    virtual future<> send_append_entries(server_id id, const append_request_send& append_request) = 0;

    // Send a reply to an append_request. The returned future
    // resolves when message is sent. It does not mean it was
    // received.
    virtual future<> send_append_entries_reply(server_id id, const append_reply& reply) = 0;

    // Send a vote request. The returned future
    // resolves when message is sent. It does not mean it was
    // received.
    virtual future<> send_vote_request(server_id id, const vote_request& vote_request) = 0;

    // Sends a reply to a vote request. The returned future
    // resolves when message is sent. It does not mean it was
    // received.
    virtual future<> send_vote_reply(server_id id, const vote_reply& vote_reply) = 0;

    // When a new server is learn this function is called with the
    // info about the server.
    virtual void add_server(server_id id, server_info info) = 0;

    // When a server is removed from local config this call is
    // executed.
    virtual void remove_server(server_id id) = 0;

    // Stop the RPC instance by aborting the work that can be
    // aborted and waiting for all the rest to complete any
    // unfinished send operation may return an error after this
    // function is called.
    virtual future<> abort() = 0;
private:
    friend rpc_server;
};

// Each Raft server is a receiver of RPC messages.
// Defines the API specific to receiving RPC input.
class rpc_server {
public:
    virtual ~rpc_server() {};

    // This function is called by append_entries RPC
    virtual void append_entries(server_id from, append_request_recv append_request) = 0;

    // This function is called by append_entries_reply RPC
    virtual void append_entries_reply(server_id from, append_reply reply) = 0;

    // This function is called to handle RequestVote RPC.
    virtual void request_vote(server_id from, vote_request vote_request) = 0;
    // Handle response to RequestVote RPC
    virtual void request_vote_reply(server_id from, vote_reply vote_reply) = 0;

    // Apply incoming snapshot, future resolves when application is complete
    virtual future<> apply_snapshot(server_id from, install_snapshot snp) = 0;

    // Update RPC implementation with this client as
    // the receiver of RPC input.
    void set_rpc_server(class rpc *rpc) { rpc->_client = this; }
};

// This class represents persistent storage state. If any of the
// function returns an error the Raft instance will be aborted.
class storage {
public:
    virtual ~storage() {}

    // Persist given term and vote.
    // Can be called concurrently with other save-* functions in
    // the storage and with itself but an implementation has to
    // make sure that the result is returned back in the calling order.
    virtual future<> store_term_and_vote(term_t term, server_id vote) = 0;

    // Load persisted term and vote.
    // Called during Raft server initialization only, is not run
    // in parallel with store.
    virtual future<std::pair<term_t, server_id>> load_term_and_vote() = 0;

    // Persist given snapshot and drop all but 'preserve_log_entries'
    // entries from the Raft log starting from the beginning.
    // This can overwrite a previously persisted snapshot.
    // Is called only after the previous invocation completes.
    // In other words, it's the caller's responsibility to serialize
    // calls to this function. Can be called in parallel with
    // store_log_entries() but snap.index should belong to an already
    // persisted entry.
    virtual future<> store_snapshot(const snapshot& snap, size_t preserve_log_entries) = 0;

    // Load a saved snapshot.
    // This only loads it into memory, but does not apply yet. To
    // apply call 'state_machine::load_snapshot(snapshot::id)'
    // Called during Raft server initialization only, should not
    // run in parallel with store.
    virtual future<snapshot> load_snapshot() = 0;

    // Persist given log entries.
    // Can be called without waiting for previous call to resolve,
    // but internally all writes should be serialized into forming
    // one contiguous log that holds entries in order of the
    // function invocation.
    virtual future<> store_log_entries(const std::vector<log_entry_ptr>& entries) = 0;

    // Load saved Raft log. Called during Raft server
    // initialization only, should not run in parallel with store.
    virtual future<log_entries> load_log() = 0;

    // Truncate all entries with an index greater or equal than
    // the given index in the log and persist the truncation. Can be
    // called in parallel with store_log_entries() but internally
    // should be linearized vs store_log_entries():
    // store_log_entries() called after truncate_log() should wait
    // for truncation to complete internally before persisting its
    // entries.
    virtual future<> truncate_log(index_t idx) = 0;

    // Stop the storage instance by aborting the work that can be
    // aborted and waiting for all the rest to complete any
    // unfinished store/load operation may return an error after
    // this function is called.
    virtual future<> abort() = 0;
};

// To support many Raft groups per server, Seastar Raft
// extends original Raft with a shared failure detector.
// It is used instead of empty AppendEntries PRCs in idle
// cluster.
// This allows multiple Raft groups to share heartbeat traffic.
class failure_detector {
public:
    virtual ~failure_detector() {}
    // Called by each server on each tick, which defaults to 10
    // per second. Should return true if the server is
    // alive. False results may impact liveness.
    virtual bool is_alive(server_id server) = 0;
};

} // namespace raft

