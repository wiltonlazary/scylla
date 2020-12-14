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

#include <seastar/core/semaphore.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/future.hh>

using namespace seastar;

namespace cql_transport { class cql_server; }
class database;
namespace auth { class service; }
namespace service { class migration_notifier; }
namespace gms { class gossiper; }
namespace cql3 { class query_processor; }

namespace cql_transport {

class controller {
    std::unique_ptr<distributed<cql_transport::cql_server>> _server;
    semaphore _ops_sem; /* protects start/stop operations on _server */
    bool _stopped = false;

    distributed<database>& _db;
    sharded<auth::service>& _auth_service;
    sharded<service::migration_notifier>& _mnotifier;
    gms::gossiper& _gossiper;
    sharded<cql3::query_processor>& _qp;

    future<> set_cql_ready(bool ready);
    future<> do_start_server();
    future<> do_stop_server();

public:
    controller(distributed<database>&, sharded<auth::service>&, sharded<service::migration_notifier>&, gms::gossiper&, sharded<cql3::query_processor>&);
    future<> start_server();
    future<> stop_server();
    future<> stop();
    future<bool> is_server_running();
};

} // namespace cql_transport
