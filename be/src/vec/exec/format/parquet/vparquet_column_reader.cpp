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

#include "vparquet_column_reader.h"

#include <common/status.h>
#include <gen_cpp/parquet_types.h>

#include "schema_desc.h"
#include "vec/columns/column_array.h"
#include "vec/columns/column_map.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_struct.h"
#include "vec/data_types/data_type_array.h"
#include "vec/data_types/data_type_map.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_struct.h"
#include "vparquet_column_chunk_reader.h"

namespace doris::vectorized {

static void fill_struct_null_map(FieldSchema* field, NullMap& null_map,
                                 const std::vector<level_t>& rep_levels,
                                 const std::vector<level_t>& def_levels) {
    size_t num_levels = def_levels.size();
    DCHECK_EQ(num_levels, rep_levels.size());
    size_t origin_size = null_map.size();
    null_map.resize(origin_size + num_levels);
    size_t pos = origin_size;
    for (size_t i = 0; i < num_levels; ++i) {
        // skip the levels affect its ancestor or its descendants
        if (def_levels[i] < field->repeated_parent_def_level ||
            rep_levels[i] > field->repetition_level) {
            continue;
        }
        if (def_levels[i] >= field->definition_level) {
            null_map[pos++] = 0;
        } else {
            null_map[pos++] = 1;
        }
    }
    null_map.resize(pos + 1);
}

static void fill_array_offset(FieldSchema* field, ColumnArray::Offsets64& offsets_data,
                              NullMap* null_map_ptr, const std::vector<level_t>& rep_levels,
                              const std::vector<level_t>& def_levels) {
    size_t num_levels = rep_levels.size();
    DCHECK_EQ(num_levels, def_levels.size());
    size_t origin_size = offsets_data.size();
    offsets_data.resize(origin_size + num_levels);
    if (null_map_ptr != nullptr) {
        null_map_ptr->resize(origin_size + num_levels);
    }
    size_t offset_pos = origin_size - 1;
    for (size_t i = 0; i < num_levels; ++i) {
        // skip the levels affect its ancestor or its descendants
        if (def_levels[i] < field->repeated_parent_def_level ||
            rep_levels[i] > field->repetition_level) {
            continue;
        }
        if (rep_levels[i] == field->repetition_level) {
            offsets_data[offset_pos]++;
            continue;
        }
        offset_pos++;
        offsets_data[offset_pos] = offsets_data[offset_pos - 1];
        if (def_levels[i] >= field->definition_level) {
            offsets_data[offset_pos]++;
        }
        if (def_levels[i] >= field->definition_level - 1) {
            (*null_map_ptr)[offset_pos] = 0;
        } else {
            (*null_map_ptr)[offset_pos] = 1;
        }
    }
    offsets_data.resize(offset_pos + 1);
    if (null_map_ptr != nullptr) {
        null_map_ptr->resize(offset_pos + 1);
    }
}

Status ParquetColumnReader::create(io::FileReaderSPtr file, FieldSchema* field,
                                   const tparquet::RowGroup& row_group,
                                   const std::vector<RowRange>& row_ranges, cctz::time_zone* ctz,
                                   std::unique_ptr<ParquetColumnReader>& reader,
                                   size_t max_buf_size) {
    if (field->type.type == TYPE_ARRAY) {
        std::unique_ptr<ParquetColumnReader> element_reader;
        RETURN_IF_ERROR(create(file, &field->children[0], row_group, row_ranges, ctz,
                               element_reader, max_buf_size));
        element_reader->set_nested_column();
        ArrayColumnReader* array_reader = new ArrayColumnReader(row_ranges, ctz);
        RETURN_IF_ERROR(array_reader->init(std::move(element_reader), field));
        reader.reset(array_reader);
    } else if (field->type.type == TYPE_MAP) {
        std::unique_ptr<ParquetColumnReader> key_reader;
        std::unique_ptr<ParquetColumnReader> value_reader;
        RETURN_IF_ERROR(create(file, &field->children[0].children[0], row_group, row_ranges, ctz,
                               key_reader, max_buf_size));
        RETURN_IF_ERROR(create(file, &field->children[0].children[1], row_group, row_ranges, ctz,
                               value_reader, max_buf_size));
        key_reader->set_nested_column();
        value_reader->set_nested_column();
        MapColumnReader* map_reader = new MapColumnReader(row_ranges, ctz);
        RETURN_IF_ERROR(map_reader->init(std::move(key_reader), std::move(value_reader), field));
        reader.reset(map_reader);
    } else if (field->type.type == TYPE_STRUCT) {
        std::vector<std::unique_ptr<ParquetColumnReader>> child_readers;
        for (int i = 0; i < field->children.size(); ++i) {
            std::unique_ptr<ParquetColumnReader> child_reader;
            RETURN_IF_ERROR(create(file, &field->children[i], row_group, row_ranges, ctz,
                                   child_reader, max_buf_size));
            child_reader->set_nested_column();
            child_readers.emplace_back(std::move(child_reader));
        }
        StructColumnReader* struct_reader = new StructColumnReader(row_ranges, ctz);
        RETURN_IF_ERROR(struct_reader->init(std::move(child_readers), field));
        reader.reset(struct_reader);
    } else {
        const tparquet::ColumnChunk& chunk = row_group.columns[field->physical_column_index];
        ScalarColumnReader* scalar_reader = new ScalarColumnReader(row_ranges, chunk, ctz);
        RETURN_IF_ERROR(scalar_reader->init(file, field, max_buf_size));
        reader.reset(scalar_reader);
    }
    return Status::OK();
}

void ParquetColumnReader::_generate_read_ranges(int64_t start_index, int64_t end_index,
                                                std::list<RowRange>& read_ranges) {
    if (_nested_column) {
        read_ranges.emplace_back(start_index, end_index);
        return;
    }
    int index = _row_range_index;
    while (index < _row_ranges.size()) {
        const RowRange& read_range = _row_ranges[index];
        if (read_range.last_row <= start_index) {
            index++;
            _row_range_index++;
            continue;
        }
        if (read_range.first_row >= end_index) {
            break;
        }
        int64_t start = read_range.first_row < start_index ? start_index : read_range.first_row;
        int64_t end = read_range.last_row < end_index ? read_range.last_row : end_index;
        read_ranges.emplace_back(start, end);
        index++;
    }
}

Status ScalarColumnReader::init(io::FileReaderSPtr file, FieldSchema* field, size_t max_buf_size) {
    _field_schema = field;
    auto& chunk_meta = _chunk_meta.meta_data;
    int64_t chunk_start = chunk_meta.__isset.dictionary_page_offset
                                  ? chunk_meta.dictionary_page_offset
                                  : chunk_meta.data_page_offset;
    size_t chunk_len = chunk_meta.total_compressed_size;
    _stream_reader = std::make_unique<io::BufferedFileStreamReader>(
            file, chunk_start, chunk_len, std::min(chunk_len, max_buf_size));
    _chunk_reader =
            std::make_unique<ColumnChunkReader>(_stream_reader.get(), &_chunk_meta, field, _ctz);
    RETURN_IF_ERROR(_chunk_reader->init());
    return Status::OK();
}

Status ScalarColumnReader::_skip_values(size_t num_values) {
    if (num_values == 0) {
        return Status::OK();
    }
    if (_chunk_reader->max_def_level() > 0) {
        LevelDecoder& def_decoder = _chunk_reader->def_level_decoder();
        size_t skipped = 0;
        size_t null_size = 0;
        size_t nonnull_size = 0;
        while (skipped < num_values) {
            level_t def_level = -1;
            size_t loop_skip = def_decoder.get_next_run(&def_level, num_values - skipped);
            if (loop_skip == 0) {
                continue;
            }
            if (def_level == 0) {
                null_size += loop_skip;
            } else {
                nonnull_size += loop_skip;
            }
            skipped += loop_skip;
        }
        if (null_size > 0) {
            RETURN_IF_ERROR(_chunk_reader->skip_values(null_size, false));
        }
        if (nonnull_size > 0) {
            RETURN_IF_ERROR(_chunk_reader->skip_values(nonnull_size, true));
        }
    } else {
        RETURN_IF_ERROR(_chunk_reader->skip_values(num_values));
    }
    return Status::OK();
}

Status ScalarColumnReader::_read_values(size_t num_values, ColumnPtr& doris_column,
                                        DataTypePtr& type, ColumnSelectVector& select_vector,
                                        bool is_dict_filter) {
    if (num_values == 0) {
        return Status::OK();
    }
    MutableColumnPtr data_column;
    std::vector<uint16_t> null_map;
    NullMap* map_data_column = nullptr;
    if (doris_column->is_nullable()) {
        SCOPED_RAW_TIMER(&_decode_null_map_time);
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        data_column = nullable_column->get_nested_column_ptr();
        map_data_column = &(nullable_column->get_null_map_data());
        if (_chunk_reader->max_def_level() > 0) {
            LevelDecoder& def_decoder = _chunk_reader->def_level_decoder();
            size_t has_read = 0;
            bool prev_is_null = true;
            while (has_read < num_values) {
                level_t def_level;
                size_t loop_read = def_decoder.get_next_run(&def_level, num_values - has_read);
                if (loop_read == 0) {
                    continue;
                }
                bool is_null = def_level == 0;
                if (!(prev_is_null ^ is_null)) {
                    null_map.emplace_back(0);
                }
                size_t remaining = loop_read;
                while (remaining > USHRT_MAX) {
                    null_map.emplace_back(USHRT_MAX);
                    null_map.emplace_back(0);
                    remaining -= USHRT_MAX;
                }
                null_map.emplace_back((u_short)remaining);
                prev_is_null = is_null;
                has_read += loop_read;
            }
        }
    } else {
        if (_chunk_reader->max_def_level() > 0) {
            return Status::Corruption("Not nullable column has null values in parquet file");
        }
        data_column = doris_column->assume_mutable();
    }
    if (null_map.size() == 0) {
        size_t remaining = num_values;
        while (remaining > USHRT_MAX) {
            null_map.emplace_back(USHRT_MAX);
            null_map.emplace_back(0);
            remaining -= USHRT_MAX;
        }
        null_map.emplace_back((u_short)remaining);
    }
    {
        SCOPED_RAW_TIMER(&_decode_null_map_time);
        select_vector.set_run_length_null_map(null_map, num_values, map_data_column);
    }
    return _chunk_reader->decode_values(data_column, type, select_vector, is_dict_filter);
}

Status ScalarColumnReader::_read_nested_column(ColumnPtr& doris_column, DataTypePtr& type,
                                               ColumnSelectVector& select_vector, size_t batch_size,
                                               size_t* read_rows, bool* eof, bool is_dict_filter) {
    _rep_levels.resize(0);
    _def_levels.resize(0);
    size_t parsed_rows = 0;
    size_t remaining_values = _chunk_reader->remaining_num_values();
    bool has_rep_level = _chunk_reader->max_rep_level() > 0;
    bool has_def_level = _chunk_reader->max_def_level() > 0;

    if (has_rep_level) {
        LevelDecoder& rep_decoder = _chunk_reader->rep_level_decoder();
        while (parsed_rows <= batch_size && remaining_values > 0) {
            level_t rep_level = rep_decoder.get_next();
            if (rep_level == 0) {
                if (parsed_rows == batch_size) {
                    rep_decoder.rewind_one();
                    break;
                }
                parsed_rows++;
            }
            _rep_levels.emplace_back(rep_level);
            remaining_values--;
        }
    } else {
        parsed_rows = std::min(remaining_values, batch_size);
        remaining_values -= parsed_rows;
        _rep_levels.resize(parsed_rows);
        for (size_t i = 0; i < parsed_rows; ++i) {
            _rep_levels[i] = 0;
        }
    }
    size_t parsed_values = _chunk_reader->remaining_num_values() - remaining_values;
    _def_levels.resize(parsed_values);
    if (has_def_level) {
        _chunk_reader->def_level_decoder().get_levels(&_def_levels[0], parsed_values);
    } else {
        for (size_t i = 0; i < parsed_values; ++i) {
            _def_levels[i] = 0;
        }
    }

    MutableColumnPtr data_column;
    std::vector<uint16_t> null_map;
    NullMap* map_data_column = nullptr;
    if (doris_column->is_nullable()) {
        SCOPED_RAW_TIMER(&_decode_null_map_time);
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        data_column = nullable_column->get_nested_column_ptr();
        map_data_column = &(nullable_column->get_null_map_data());
    } else {
        if (_field_schema->is_nullable) {
            return Status::Corruption("Not nullable column has null values in parquet file");
        }
        data_column = doris_column->assume_mutable();
    }
    size_t has_read = 0;
    size_t ancestor_nulls = 0;
    null_map.emplace_back(0);
    bool prev_is_null = false;
    while (has_read < parsed_values) {
        level_t def_level = _def_levels[has_read++];
        size_t loop_read = 1;
        while (has_read < parsed_values && _def_levels[has_read] == def_level) {
            has_read++;
            loop_read++;
        }
        if (def_level < _field_schema->repeated_parent_def_level) {
            // when def_level is less than repeated_parent_def_level, it means that level
            // will affect its ancestor.
            ancestor_nulls += loop_read;
            continue;
        }
        bool is_null = def_level < _field_schema->definition_level;
        if (prev_is_null == is_null) {
            if (USHRT_MAX - null_map.back() >= loop_read) {
                null_map.back() += loop_read;
            }
        } else {
            if (!(prev_is_null ^ is_null)) {
                null_map.emplace_back(0);
            }
            size_t remaining = loop_read;
            while (remaining > USHRT_MAX) {
                null_map.emplace_back(USHRT_MAX);
                null_map.emplace_back(0);
                remaining -= USHRT_MAX;
            }
            null_map.emplace_back((u_short)remaining);
            prev_is_null = is_null;
        }
    }

    size_t num_values = parsed_values - ancestor_nulls;
    if (num_values > 0) {
        SCOPED_RAW_TIMER(&_decode_null_map_time);
        select_vector.set_run_length_null_map(null_map, num_values, map_data_column);
    }
    RETURN_IF_ERROR(_chunk_reader->decode_values(data_column, type, select_vector, is_dict_filter));
    if (ancestor_nulls != 0) {
        _chunk_reader->skip_values(ancestor_nulls, false);
    }

    *read_rows = parsed_rows;
    if (_chunk_reader->remaining_num_values() == 0 && !_chunk_reader->has_next_page()) {
        *eof = true;
    }
    return Status::OK();
}
Status ScalarColumnReader::read_dict_values_to_column(MutableColumnPtr& doris_column,
                                                      bool* has_dict) {
    bool loaded;
    RETURN_IF_ERROR(_try_load_dict_page(&loaded, has_dict));
    if (loaded && has_dict) {
        return _chunk_reader->read_dict_values_to_column(doris_column);
    }
    return Status::OK();
}

Status ScalarColumnReader::get_dict_codes(const ColumnString* column_string,
                                          std::vector<int32_t>* dict_codes) {
    return _chunk_reader->get_dict_codes(column_string, dict_codes);
}

MutableColumnPtr ScalarColumnReader::convert_dict_column_to_string_column(
        const ColumnInt32* dict_column) {
    return _chunk_reader->convert_dict_column_to_string_column(dict_column);
}

Status ScalarColumnReader::_try_load_dict_page(bool* loaded, bool* has_dict) {
    *loaded = false;
    *has_dict = false;
    if (_chunk_reader->remaining_num_values() == 0) {
        if (!_chunk_reader->has_next_page()) {
            *loaded = false;
            return Status::OK();
        }
        RETURN_IF_ERROR(_chunk_reader->next_page());
        *loaded = true;
        *has_dict = _chunk_reader->has_dict();
    }
    return Status::OK();
}

Status ScalarColumnReader::read_column_data(ColumnPtr& doris_column, DataTypePtr& type,
                                            ColumnSelectVector& select_vector, size_t batch_size,
                                            size_t* read_rows, bool* eof, bool is_dict_filter) {
    if (_chunk_reader->remaining_num_values() == 0) {
        if (!_chunk_reader->has_next_page()) {
            *eof = true;
            *read_rows = 0;
            return Status::OK();
        }
        RETURN_IF_ERROR(_chunk_reader->next_page());
    }
    if (_nested_column) {
        RETURN_IF_ERROR(_chunk_reader->load_page_data_idempotent());
        return _read_nested_column(doris_column, type, select_vector, batch_size, read_rows, eof,
                                   is_dict_filter);
    }

    // generate the row ranges that should be read
    std::list<RowRange> read_ranges;
    _generate_read_ranges(_current_row_index,
                          _current_row_index + _chunk_reader->remaining_num_values(), read_ranges);
    if (read_ranges.size() == 0) {
        // skip the whole page
        _current_row_index += _chunk_reader->remaining_num_values();
        RETURN_IF_ERROR(_chunk_reader->skip_page());
        *read_rows = 0;
    } else {
        bool skip_whole_batch = false;
        // Determining whether to skip page or batch will increase the calculation time.
        // When the filtering effect is greater than 60%, it is possible to skip the page or batch.
        if (select_vector.has_filter() && select_vector.filter_ratio() > 0.6) {
            // lazy read
            size_t remaining_num_values = 0;
            for (auto& range : read_ranges) {
                remaining_num_values += range.last_row - range.first_row;
            }
            if (batch_size >= remaining_num_values &&
                select_vector.can_filter_all(remaining_num_values)) {
                // We can skip the whole page if the remaining values is filtered by predicate columns
                select_vector.skip(remaining_num_values);
                _current_row_index += _chunk_reader->remaining_num_values();
                RETURN_IF_ERROR(_chunk_reader->skip_page());
                *read_rows = remaining_num_values;
                if (!_chunk_reader->has_next_page()) {
                    *eof = true;
                }
                return Status::OK();
            }
            skip_whole_batch =
                    batch_size <= remaining_num_values && select_vector.can_filter_all(batch_size);
            if (skip_whole_batch) {
                select_vector.skip(batch_size);
            }
        }
        // load page data to decode or skip values
        RETURN_IF_ERROR(_chunk_reader->load_page_data_idempotent());
        size_t has_read = 0;
        for (auto& range : read_ranges) {
            // generate the skipped values
            size_t skip_values = range.first_row - _current_row_index;
            RETURN_IF_ERROR(_skip_values(skip_values));
            _current_row_index += skip_values;
            // generate the read values
            size_t read_values =
                    std::min((size_t)(range.last_row - range.first_row), batch_size - has_read);
            if (skip_whole_batch) {
                RETURN_IF_ERROR(_skip_values(read_values));
            } else {
                RETURN_IF_ERROR(_read_values(read_values, doris_column, type, select_vector,
                                             is_dict_filter));
            }
            has_read += read_values;
            _current_row_index += read_values;
            if (has_read == batch_size) {
                break;
            }
        }
        *read_rows = has_read;
    }

    if (_chunk_reader->remaining_num_values() == 0 && !_chunk_reader->has_next_page()) {
        *eof = true;
    }
    return Status::OK();
}

Status ArrayColumnReader::init(std::unique_ptr<ParquetColumnReader> element_reader,
                               FieldSchema* field) {
    _field_schema = field;
    _element_reader = std::move(element_reader);
    return Status::OK();
}

Status ArrayColumnReader::read_column_data(ColumnPtr& doris_column, DataTypePtr& type,
                                           ColumnSelectVector& select_vector, size_t batch_size,
                                           size_t* read_rows, bool* eof, bool is_dict_filter) {
    MutableColumnPtr data_column;
    NullMap* null_map_ptr = nullptr;
    if (doris_column->is_nullable()) {
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        null_map_ptr = &nullable_column->get_null_map_data();
        data_column = nullable_column->get_nested_column_ptr();
    } else {
        if (_field_schema->is_nullable) {
            return Status::Corruption("Not nullable column has null values in parquet file");
        }
        data_column = doris_column->assume_mutable();
    }

    // read nested column
    RETURN_IF_ERROR(_element_reader->read_column_data(
            static_cast<ColumnArray&>(*data_column).get_data_ptr(),
            const_cast<DataTypePtr&>(
                    (reinterpret_cast<const DataTypeArray*>(remove_nullable(type).get()))
                            ->get_nested_type()),
            select_vector, batch_size, read_rows, eof, is_dict_filter));
    if (*read_rows == 0) {
        return Status::OK();
    }

    // fill offset and null map
    fill_array_offset(_field_schema, static_cast<ColumnArray&>(*data_column).get_offsets(),
                      null_map_ptr, _element_reader->get_rep_level(),
                      _element_reader->get_def_level());

    return Status::OK();
}

Status MapColumnReader::init(std::unique_ptr<ParquetColumnReader> key_reader,
                             std::unique_ptr<ParquetColumnReader> value_reader,
                             FieldSchema* field) {
    _field_schema = field;
    _key_reader = std::move(key_reader);
    _value_reader = std::move(value_reader);
    return Status::OK();
}

Status MapColumnReader::read_column_data(ColumnPtr& doris_column, DataTypePtr& type,
                                         ColumnSelectVector& select_vector, size_t batch_size,
                                         size_t* read_rows, bool* eof, bool is_dict_filter) {
    MutableColumnPtr data_column;
    NullMap* null_map_ptr = nullptr;
    if (doris_column->is_nullable()) {
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        null_map_ptr = &nullable_column->get_null_map_data();
        data_column = nullable_column->get_nested_column_ptr();
    } else {
        if (_field_schema->is_nullable) {
            return Status::Corruption("Not nullable column has null values in parquet file");
        }
        data_column = doris_column->assume_mutable();
    }

    auto& map = static_cast<ColumnMap&>(*data_column);
    DataTypePtr& key_type = const_cast<DataTypePtr&>(
            reinterpret_cast<const DataTypeMap*>(remove_nullable(type).get())->get_key_type());
    DataTypePtr& value_type = const_cast<DataTypePtr&>(
            reinterpret_cast<const DataTypeMap*>(remove_nullable(type).get())->get_value_type());
    ColumnPtr& key_column = map.get_keys_ptr();
    ColumnPtr& value_column = map.get_values_ptr();

    size_t key_rows = 0;
    size_t value_rows = 0;
    bool key_eof = false;
    bool value_eof = false;
    RETURN_IF_ERROR(_key_reader->read_column_data(key_column, key_type, select_vector, batch_size,
                                                  &key_rows, &key_eof, is_dict_filter));
    select_vector.reset();
    RETURN_IF_ERROR(_value_reader->read_column_data(value_column, value_type, select_vector,
                                                    batch_size, &value_rows, &value_eof,
                                                    is_dict_filter));
    DCHECK_EQ(key_rows, value_rows);
    DCHECK_EQ(key_eof, value_eof);
    *read_rows = key_rows;
    *eof = key_eof;

    if (*read_rows == 0) {
        return Status::OK();
    }

    // fill offset and null map
    fill_array_offset(_field_schema, map.get_offsets(), null_map_ptr, _key_reader->get_rep_level(),
                      _key_reader->get_def_level());

    return Status::OK();
}

Status StructColumnReader::init(std::vector<std::unique_ptr<ParquetColumnReader>>&& child_readers,
                                FieldSchema* field) {
    _field_schema = field;
    _child_readers = std::move(child_readers);
    return Status::OK();
}
Status StructColumnReader::read_column_data(ColumnPtr& doris_column, DataTypePtr& type,
                                            ColumnSelectVector& select_vector, size_t batch_size,
                                            size_t* read_rows, bool* eof, bool is_dict_filter) {
    MutableColumnPtr data_column;
    NullMap* null_map_ptr = nullptr;
    if (doris_column->is_nullable()) {
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        null_map_ptr = &nullable_column->get_null_map_data();
        data_column = nullable_column->get_nested_column_ptr();
    } else {
        if (_field_schema->is_nullable) {
            return Status::Corruption("Not nullable column has null values in parquet file");
        }
        data_column = doris_column->assume_mutable();
    }

    auto& doris_struct = static_cast<ColumnStruct&>(*data_column);
    if (_child_readers.size() != doris_struct.tuple_size()) {
        return Status::InternalError("Wrong number of struct fields");
    }
    const DataTypeStruct* doris_struct_type =
            reinterpret_cast<const DataTypeStruct*>(remove_nullable(type).get());
    for (int i = 0; i < doris_struct.tuple_size(); ++i) {
        ColumnPtr& doris_field = doris_struct.get_column_ptr(i);
        DataTypePtr& doris_type = const_cast<DataTypePtr&>(doris_struct_type->get_element(i));
        select_vector.reset();
        size_t loop_rows = 0;
        bool loop_eof = false;
        _child_readers[i]->read_column_data(doris_field, doris_type, select_vector, batch_size,
                                            &loop_rows, &loop_eof, is_dict_filter);
        if (i != 0) {
            DCHECK_EQ(*read_rows, loop_rows);
            DCHECK_EQ(*eof, loop_eof);
        } else {
            *read_rows = loop_rows;
            *eof = loop_eof;
        }
    }

    if (null_map_ptr != nullptr) {
        fill_struct_null_map(_field_schema, *null_map_ptr, _child_readers[0]->get_rep_level(),
                             _child_readers[0]->get_def_level());
    }

    return Status::OK();
}

}; // namespace doris::vectorized
