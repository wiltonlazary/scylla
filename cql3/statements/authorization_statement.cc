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
 */

/*
 * Copyright 2016 ScyllaDB
 *
 * Modified by ScyllaDB
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

#include "authorization_statement.hh"
#include "transport/messages/result_message.hh"

uint32_t cql3::statements::authorization_statement::get_bound_terms() const {
    return 0;
}

bool cql3::statements::authorization_statement::depends_on_keyspace(
                const sstring& ks_name) const {
    return false;
}

bool cql3::statements::authorization_statement::depends_on_column_family(
                const sstring& cf_name) const {
    return false;
}

void cql3::statements::authorization_statement::validate(
                service::storage_proxy&,
                const service::client_state& state) const {
}

future<> cql3::statements::authorization_statement::check_access(service::storage_proxy& proxy, const service::client_state& state) const {
    return make_ready_future<>();
}

void cql3::statements::authorization_statement::maybe_correct_resource(auth::resource& resource, const service::client_state& state){
    if (resource.kind() == auth::resource_kind::data) {
        const auto data_view = auth::data_resource_view(resource);
        const auto keyspace = data_view.keyspace();
        const auto table = data_view.table();

        if (table && keyspace->empty()) {
            resource = auth::make_data_resource(state.get_keyspace(), *table);
        }
    }
}

