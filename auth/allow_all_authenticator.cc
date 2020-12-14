/*
 * Copyright (C) 2017 ScyllaDB
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

#include "auth/allow_all_authenticator.hh"

#include "service/migration_manager.hh"
#include "utils/class_registrator.hh"

namespace auth {

constexpr std::string_view allow_all_authenticator_name("org.apache.cassandra.auth.AllowAllAuthenticator");

// To ensure correct initialization order, we unfortunately need to use a string literal.
static const class_registrator<
        authenticator,
        allow_all_authenticator,
        cql3::query_processor&,
        ::service::migration_manager&> registration("org.apache.cassandra.auth.AllowAllAuthenticator");

}
