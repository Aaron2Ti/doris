// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/exprs/table_function/table_function_factory.h"

#include "common/object_pool.h"

namespace doris::vectorized {

template <typename TableFunctionType>
struct TableFunctionCreator {
    TableFunction* operator()() { return new TableFunctionType(); }
};

template <>
struct TableFunctionCreator<VExplodeJsonArrayTableFunction> {
    ExplodeJsonArrayType type;
    TableFunction* operator()() const { return new VExplodeJsonArrayTableFunction(type); }
};

inline auto VExplodeJsonArrayIntCreator =
        TableFunctionCreator<VExplodeJsonArrayTableFunction> {ExplodeJsonArrayType::INT};
inline auto VExplodeJsonArrayDoubleCreator =
        TableFunctionCreator<VExplodeJsonArrayTableFunction> {ExplodeJsonArrayType::DOUBLE};
inline auto VExplodeJsonArrayStringCreator =
        TableFunctionCreator<VExplodeJsonArrayTableFunction> {ExplodeJsonArrayType::STRING};

const std::unordered_map<std::string, std::function<TableFunction*()>>
        TableFunctionFactory::_function_map {
                {"explode_split", TableFunctionCreator<VExplodeSplitTableFunction>()},
                {"explode_numbers", TableFunctionCreator<VExplodeNumbersTableFunction>()},
                {"explode_json_array_int", VExplodeJsonArrayIntCreator},
                {"explode_json_array_double", VExplodeJsonArrayDoubleCreator},
                {"explode_json_array_string", VExplodeJsonArrayStringCreator},
                {"explode_bitmap", TableFunctionCreator<VExplodeBitmapTableFunction>()},
                {"explode", TableFunctionCreator<VExplodeTableFunction> {}}};

Status TableFunctionFactory::get_fn(const std::string& fn_name_raw, ObjectPool* pool,
                                    TableFunction** fn) {
    auto match_suffix = [](const std::string& name, const std::string& suffix) -> bool {
        if (name.length() < suffix.length()) {
            return false;
        }
        return name.substr(name.length() - suffix.length()) == suffix;
    };

    auto remove_suffix = [](const std::string& name, const std::string& suffix) -> std::string {
        return name.substr(0, name.length() - suffix.length());
    };

    bool is_outer = match_suffix(fn_name_raw, COMBINATOR_SUFFIX_OUTER);
    std::string fn_name_real =
            is_outer ? remove_suffix(fn_name_raw, COMBINATOR_SUFFIX_OUTER) : fn_name_raw;

    auto fn_iterator = _function_map.find(fn_name_real);
    if (fn_iterator != _function_map.end()) {
        *fn = pool->add(fn_iterator->second());
        if (is_outer) {
            (*fn)->set_outer();
        }

        return Status::OK();
    }

    return Status::NotSupported("Table function {} is not support", fn_name_raw);
}

} // namespace doris::vectorized
