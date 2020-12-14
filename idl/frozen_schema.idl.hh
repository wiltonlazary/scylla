/*
 * Copyright 2016 ScyllaDB
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

class canonical_mutation final {
    bytes representation();
};

class schema_mutations {
    canonical_mutation columnfamilies_canonical_mutation();
    canonical_mutation columns_canonical_mutation();
    bool is_view()[[version 1.6]];
    std::optional<canonical_mutation> indices_canonical_mutation()[[version 2.0]];
    std::optional<canonical_mutation> dropped_columns_canonical_mutation()[[version 2.0]];
    std::optional<canonical_mutation> scylla_tables_canonical_mutation()[[version 2.0]];
    std::optional<canonical_mutation> view_virtual_columns_canonical_mutation()[[version 2.4]];
    std::optional<canonical_mutation> computed_columns_canonical_mutation()[[version 3.2]];
};

class schema stub [[writable]] {
    utils::UUID version;
    schema_mutations mutations;
};

class frozen_schema final {
    bytes representation();
};
