/*
 * Copyright (C) 2019 pengjian.uestc @ gmail.com
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

#include "seastar/core/future.hh"
#include "seastar/core/shared_ptr.hh"
#include "seastar/core/distributed.hh"

using namespace seastar;

namespace db {
class config;
};

namespace redis {
class query_processor;
}

namespace redis_transport {
class redis_server;
}

namespace auth {
class service;
}

namespace service {
class storage_proxy;
}

class database;

class redis_service {
    distributed<redis::query_processor> _query_processor;
    shared_ptr<distributed<redis_transport::redis_server>> _server;
private:
    future<> listen(distributed<auth::service>& auth_service, db::config& cfg);
public:
    redis_service();
    ~redis_service();
    future<> init(distributed<service::storage_proxy>& proxy, distributed<database>& db, distributed<auth::service>& auth_service, db::config& cfg);
    future<> stop();
};
