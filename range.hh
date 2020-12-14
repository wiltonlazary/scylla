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

#include "interval.hh"

// range.hh is deprecated and should be replaced with interval.hh


template <typename T>
using range_bound = interval_bound<T>;

template <typename T>
using nonwrapping_range = interval<T>;

template <typename T>
using wrapping_range = wrapping_interval<T>;

template <typename T>
using range = wrapping_interval<T>;

template <template<typename> typename T, typename U>
concept Range = Interval<T, U>;
