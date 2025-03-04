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

#pragma once

#include "common/status.h"
#include "process_hash_table_probe.h"
#include "runtime/thread_context.h" // IWYU pragma: keep
#include "util/simd/bits.h"
#include "vec/exprs/vexpr_context.h"
#include "vhash_join_node.h"

namespace doris::vectorized {

template <int JoinOpType>
ProcessHashTableProbe<JoinOpType>::ProcessHashTableProbe(HashJoinProbeContext* join_context,
                                                         int batch_size)
        : _join_context(join_context),
          _batch_size(batch_size),
          _build_blocks(*join_context->_build_blocks),
          _tuple_is_null_left_flags(
                  join_context->_is_outer_join
                          ? &(reinterpret_cast<ColumnUInt8&>(
                                      **join_context->_tuple_is_null_left_flag_column)
                                      .get_data())
                          : nullptr),
          _tuple_is_null_right_flags(
                  join_context->_is_outer_join
                          ? &(reinterpret_cast<ColumnUInt8&>(
                                      **join_context->_tuple_is_null_right_flag_column)
                                      .get_data())
                          : nullptr),
          _rows_returned_counter(join_context->_rows_returned_counter),
          _search_hashtable_timer(join_context->_search_hashtable_timer),
          _build_side_output_timer(join_context->_build_side_output_timer),
          _probe_side_output_timer(join_context->_probe_side_output_timer),
          _probe_process_hashtable_timer(join_context->_probe_process_hashtable_timer) {}

template <int JoinOpType>
void ProcessHashTableProbe<JoinOpType>::build_side_output_column(
        MutableColumns& mcol, const std::vector<bool>& output_slot_flags, int size,
        bool have_other_join_conjunct) {
    SCOPED_TIMER(_build_side_output_timer);
    constexpr auto is_semi_anti_join = JoinOpType == TJoinOp::RIGHT_ANTI_JOIN ||
                                       JoinOpType == TJoinOp::RIGHT_SEMI_JOIN ||
                                       JoinOpType == TJoinOp::LEFT_ANTI_JOIN ||
                                       JoinOpType == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN ||
                                       JoinOpType == TJoinOp::LEFT_SEMI_JOIN;

    constexpr auto probe_all =
            JoinOpType == TJoinOp::LEFT_OUTER_JOIN || JoinOpType == TJoinOp::FULL_OUTER_JOIN;

    if (!is_semi_anti_join || have_other_join_conjunct) {
        if (_build_blocks.size() == 1) {
            for (int i = 0; i < _right_col_len; i++) {
                auto& column = *_build_blocks[0].get_by_position(i).column;
                if (output_slot_flags[i]) {
                    mcol[i + _right_col_idx]->insert_indices_from(column, _build_block_rows.data(),
                                                                  _build_block_rows.data() + size);
                } else {
                    mcol[i + _right_col_idx]->insert_many_defaults(size);
                }
            }
        } else {
            for (int i = 0; i < _right_col_len; i++) {
                if (output_slot_flags[i]) {
                    for (int j = 0; j < size; j++) {
                        if constexpr (probe_all) {
                            if (_build_block_offsets[j] == -1) {
                                DCHECK(mcol[i + _right_col_idx]->is_nullable());
                                assert_cast<ColumnNullable*>(mcol[i + _right_col_idx].get())
                                        ->insert_default();
                            } else {
                                auto& column = *_build_blocks[_build_block_offsets[j]]
                                                        .get_by_position(i)
                                                        .column;
                                mcol[i + _right_col_idx]->insert_from(column, _build_block_rows[j]);
                            }
                        } else {
                            if (_build_block_offsets[j] == -1) {
                                // the only case to reach here:
                                // 1. left anti join with other conjuncts, and
                                // 2. equal conjuncts does not match
                                // since nullptr is emplaced back to visited_map,
                                // the output value of the build side does not matter,
                                // just insert default value
                                mcol[i + _right_col_idx]->insert_default();
                            } else {
                                auto& column = *_build_blocks[_build_block_offsets[j]]
                                                        .get_by_position(i)
                                                        .column;
                                mcol[i + _right_col_idx]->insert_from(column, _build_block_rows[j]);
                            }
                        }
                    }
                } else {
                    mcol[i + _right_col_idx]->insert_many_defaults(size);
                }
            }
        }
    }

    // Dispose right tuple is null flags columns
    if (probe_all && !have_other_join_conjunct) {
        _tuple_is_null_right_flags->resize(size);
        auto* __restrict null_data = _tuple_is_null_right_flags->data();
        for (int i = 0; i < size; ++i) {
            null_data[i] = _build_block_rows[i] == -1;
        }
    }
}

template <int JoinOpType>
void ProcessHashTableProbe<JoinOpType>::probe_side_output_column(
        MutableColumns& mcol, const std::vector<bool>& output_slot_flags, int size,
        int last_probe_index, size_t probe_size, bool all_match_one,
        bool have_other_join_conjunct) {
    SCOPED_TIMER(_probe_side_output_timer);
    auto& probe_block = *_join_context->_probe_block;
    for (int i = 0; i < output_slot_flags.size(); ++i) {
        if (output_slot_flags[i]) {
            auto& column = probe_block.get_by_position(i).column;
            if (all_match_one) {
                mcol[i]->insert_range_from(*column, last_probe_index, probe_size);
            } else {
                column->replicate(_probe_indexs.data(), size, *mcol[i]);
            }
        } else {
            mcol[i]->insert_many_defaults(size);
        }
    }

    if constexpr (JoinOpType == TJoinOp::RIGHT_OUTER_JOIN) {
        if (!have_other_join_conjunct) {
            _tuple_is_null_left_flags->resize_fill(size, 0);
        }
    }
}

template <int JoinOpType>
void ProcessHashTableProbe<JoinOpType>::_pre_serialize_key(
        const ColumnRawPtrs& key_columns, const size_t key_rows,
        std::vector<StringRef>& serialized_keys) {
    if (serialized_keys.size() < key_rows) {
        serialized_keys.resize(key_rows);
    }
    size_t max_one_row_byte_size = 0;
    for (const auto column : key_columns) {
        max_one_row_byte_size += column->get_max_row_byte_size();
    }
    size_t total_bytes = max_one_row_byte_size * key_rows;

    /// reach mem limit, don't serialize in batch
    /// If there is a very long row of data in a string column,
    /// it will result in a very larger estimated total_bytes.
    if (total_bytes > config::pre_serialize_keys_limit_bytes) {
        size_t old_probe_keys_memory_usage = 0;
        if (!_arena) {
            _arena.reset(new Arena());
        } else {
            old_probe_keys_memory_usage = _arena->size();
        }

        _arena->clear();
        size_t keys_size = key_columns.size();
        for (size_t i = 0; i < key_rows; ++i) {
            serialized_keys[i] =
                    serialize_keys_to_pool_contiguous(i, keys_size, key_columns, *_arena);
        }
        _join_context->_probe_arena_memory_usage->add(_arena->size() - old_probe_keys_memory_usage);
    } else {
        if (!_serialize_key_arena) {
            _serialize_key_arena.reset(new Arena);
        }
        if (total_bytes > _serialized_key_buffer_size) {
            _join_context->_probe_arena_memory_usage->add(-_serialized_key_buffer_size);
            _serialized_key_buffer_size = total_bytes;
            _serialize_key_arena->clear();
            _serialized_key_buffer = reinterpret_cast<uint8_t*>(
                    _serialize_key_arena->alloc(_serialized_key_buffer_size));
            _join_context->_probe_arena_memory_usage->add(_serialized_key_buffer_size);
        }

        for (size_t i = 0; i < key_rows; ++i) {
            serialized_keys[i].data =
                    reinterpret_cast<char*>(_serialized_key_buffer + i * max_one_row_byte_size);
            serialized_keys[i].size = 0;
        }

        for (const auto column : key_columns) {
            column->serialize_vec(serialized_keys, key_rows, max_one_row_byte_size);
        }
    }
}

template <int JoinOpType>
template <typename KeyGetter>
KeyGetter ProcessHashTableProbe<JoinOpType>::_init_probe_side(size_t probe_rows,
                                                              bool with_other_join_conjuncts) {
    _right_col_idx = _join_context->_is_right_semi_anti && !with_other_join_conjuncts
                             ? 0
                             : _join_context->_left_table_data_types->size();
    _right_col_len = _join_context->_right_table_data_types->size();
    _row_count_from_last_probe = 0;

    _build_block_rows.clear();
    _build_block_offsets.clear();
    _probe_indexs.clear();
    if (with_other_join_conjuncts) {
        // use in right join to change visited state after exec the vother join conjunct
        _visited_map.clear();
        _same_to_prev.clear();
        _visited_map.reserve(_batch_size * PROBE_SIDE_EXPLODE_RATE);
        _same_to_prev.reserve(_batch_size * PROBE_SIDE_EXPLODE_RATE);
    }
    _probe_indexs.reserve(_batch_size * PROBE_SIDE_EXPLODE_RATE);
    _build_block_rows.reserve(_batch_size * PROBE_SIDE_EXPLODE_RATE);
    _build_block_offsets.reserve(_batch_size * PROBE_SIDE_EXPLODE_RATE);

    KeyGetter key_getter(*_join_context->_probe_columns, _join_context->_probe_key_sz, nullptr);

    if constexpr (ColumnsHashing::IsPreSerializedKeysHashMethodTraits<KeyGetter>::value) {
        if (*_join_context->_ready_probe == false) {
            _pre_serialize_key(*_join_context->_probe_columns, probe_rows, _probe_keys);
        }
        key_getter.set_serialized_keys(_probe_keys.data());
    }

    return key_getter;
}

template <int JoinOpType>
template <bool need_null_map_for_probe, typename HashTableType, typename Keys>
void ProcessHashTableProbe<JoinOpType>::_probe_hash(const Keys& keys, HashTableType& hash_table_ctx,
                                                    ConstNullMapPtr null_map) {
    if (*_join_context->_ready_probe) {
        return;
    }
    SCOPED_TIMER(_search_hashtable_timer);
    _probe_side_hash_values.resize(keys.size());
    for (size_t k = 0; k < keys.size(); ++k) {
        if constexpr (need_null_map_for_probe) {
            if ((*null_map)[k]) {
                continue;
            }
        }
        _probe_side_hash_values[k] = hash_table_ctx.hash_table.hash(keys[k]);
    }
    *_join_context->_ready_probe = true;
}

template <int JoinOpType>
template <typename Mapped, bool with_other_join_conjuncts>
ForwardIterator<Mapped>& ProcessHashTableProbe<JoinOpType>::_probe_row_match(int& current_offset,
                                                                             int& probe_index,
                                                                             size_t& probe_size,
                                                                             bool& all_match_one) {
    auto& probe_row_match_iter =
            std::get<ForwardIterator<Mapped>>(*_join_context->_probe_row_match_iter);
    if (!probe_row_match_iter.ok()) {
        return probe_row_match_iter;
    }

    SCOPED_TIMER(_search_hashtable_timer);
    for (; probe_row_match_iter.ok() && current_offset < _batch_size; ++probe_row_match_iter) {
        _emplace_element(probe_row_match_iter->block_offset, probe_row_match_iter->row_num,
                         current_offset);
        _probe_indexs.emplace_back(probe_index);
        if constexpr (with_other_join_conjuncts) {
            _visited_map.emplace_back(&probe_row_match_iter->visited);
        }
    }

    _row_count_from_last_probe = current_offset;
    all_match_one &= (current_offset == 1);
    if (!probe_row_match_iter.ok()) {
        ++probe_index;
    }
    probe_size = 1;

    return probe_row_match_iter;
}

template <int JoinOpType>
void ProcessHashTableProbe<JoinOpType>::_emplace_element(int8_t block_offset, int32_t block_row,
                                                         int& current_offset) {
    _build_block_offsets.emplace_back(block_offset);
    _build_block_rows.emplace_back(block_row);
    current_offset++;
}

template <int JoinOpType>
template <bool need_null_map_for_probe, bool ignore_null, typename HashTableType,
          bool with_other_conjuncts, bool is_mark_join>
Status ProcessHashTableProbe<JoinOpType>::do_process(HashTableType& hash_table_ctx,
                                                     ConstNullMapPtr null_map,
                                                     MutableBlock& mutable_block,
                                                     Block* output_block, size_t probe_rows) {
    auto& probe_index = *_join_context->_probe_index;

    using KeyGetter = typename HashTableType::State;
    using Mapped = typename HashTableType::Mapped;

    KeyGetter key_getter = _init_probe_side<KeyGetter>(probe_rows, with_other_conjuncts);

    auto& mcol = mutable_block.mutable_columns();

    constexpr auto is_right_semi_anti_join =
            JoinOpType == TJoinOp::RIGHT_ANTI_JOIN || JoinOpType == TJoinOp::RIGHT_SEMI_JOIN;

    constexpr auto probe_all =
            JoinOpType == TJoinOp::LEFT_OUTER_JOIN || JoinOpType == TJoinOp::FULL_OUTER_JOIN;

    int last_probe_index = probe_index;

    int current_offset = 0;
    bool all_match_one = true;
    size_t probe_size = 0;
    auto& probe_row_match_iter = _probe_row_match<Mapped, with_other_conjuncts>(
            current_offset, probe_index, probe_size, all_match_one);

    // If not(which means it excceed batch size), probe_index is not increased and
    // remaining matched rows for the current probe row will be
    // handled in the next call of this function
    int multi_matched_output_row_count = 0;

    // Is the last sub block of splitted block
    bool is_the_last_sub_block = false;

    if (with_other_conjuncts && probe_size != 0) {
        is_the_last_sub_block = !probe_row_match_iter.ok();
        _same_to_prev.emplace_back(false);
        for (int i = 0; i < current_offset - 1; ++i) {
            _same_to_prev.emplace_back(true);
        }
    }

    const auto& keys = key_getter.get_keys();

    _probe_hash<need_null_map_for_probe, HashTableType>(keys, hash_table_ctx, null_map);

    {
        SCOPED_TIMER(_search_hashtable_timer);
        using FindResult = decltype(key_getter.find_key(hash_table_ctx.hash_table, 0, *_arena));
        FindResult empty = {nullptr, false};
        while (current_offset < _batch_size && probe_index < probe_rows) {
            if constexpr (ignore_null && need_null_map_for_probe) {
                if ((*null_map)[probe_index]) {
                    if constexpr (probe_all) {
                        // only full outer / left outer need insert the data of right table
                        _emplace_element(-1, -1, current_offset);
                        _probe_indexs.emplace_back(probe_index);

                        if constexpr (with_other_conjuncts) {
                            _same_to_prev.emplace_back(false);
                            _visited_map.emplace_back(nullptr);
                        }
                    } else {
                        all_match_one = false;
                    }
                    probe_index++;
                    continue;
                }
            }

            const auto& find_result =
                    need_null_map_for_probe && (*null_map)[probe_index]
                            ? empty
                            : key_getter.find_key_with_hash(hash_table_ctx.hash_table,
                                                            _probe_side_hash_values[probe_index],
                                                            keys[probe_index]);
            if (LIKELY(probe_index + HASH_MAP_PREFETCH_DIST < probe_rows) &&
                !(need_null_map_for_probe && (*null_map)[probe_index + HASH_MAP_PREFETCH_DIST])) {
                key_getter.template prefetch_by_hash<true>(
                        hash_table_ctx.hash_table,
                        _probe_side_hash_values[probe_index + HASH_MAP_PREFETCH_DIST]);
            }

            auto current_probe_index = probe_index;
            if constexpr (!with_other_conjuncts &&
                          (JoinOpType == TJoinOp::LEFT_ANTI_JOIN ||
                           JoinOpType == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN ||
                           JoinOpType == TJoinOp::LEFT_SEMI_JOIN)) {
                bool need_go_ahead =
                        (JoinOpType != TJoinOp::LEFT_SEMI_JOIN) ^ find_result.is_found();
                if constexpr (is_mark_join) {
                    ++current_offset;
                    assert_cast<ColumnVector<UInt8>&>(*mcol[mcol.size() - 1])
                            .get_data()
                            .template push_back(need_go_ahead);
                } else {
                    current_offset += need_go_ahead;
                }
                ++probe_index;
            } else {
                if (find_result.is_found()) {
                    auto& mapped = find_result.get_mapped();
                    auto origin_offset = current_offset;

                    // For mark join, if euqual-matched tuple count for one probe row
                    // excceeds batch size, it's difficult to implement the logic to
                    // split them into multiple sub blocks and handle them, keep the original
                    // logic for now.
                    if constexpr (is_mark_join && with_other_conjuncts) {
                        for (auto it = mapped.begin(); it.ok(); ++it) {
                            _emplace_element(it->block_offset, it->row_num, current_offset);
                            _visited_map.emplace_back(&it->visited);
                        }
                        ++probe_index;
                    } else if constexpr (with_other_conjuncts || !is_right_semi_anti_join) {
                        auto multi_match_last_offset = current_offset;
                        auto it = mapped.begin();
                        for (; it.ok() && current_offset < _batch_size; ++it) {
                            _emplace_element(it->block_offset, it->row_num, current_offset);

                            if constexpr (with_other_conjuncts) {
                                _visited_map.emplace_back(&it->visited);
                            }
                        }
                        probe_row_match_iter = it;
                        if (!it.ok()) {
                            // If all matched rows for the current probe row are handled,
                            // advance to next probe row.
                            // If not(which means it excceed batch size), probe_index is not increased and
                            // remaining matched rows for the current probe row will be
                            // handled in the next call of this function
                            ++probe_index;
                        } else if constexpr (with_other_conjuncts) {
                            multi_matched_output_row_count =
                                    current_offset - multi_match_last_offset;
                        }
                    } else {
                        ++probe_index;
                    }
                    if constexpr (std::is_same_v<Mapped, RowRefListWithFlag>) {
                        mapped.visited = true;
                    }

                    if constexpr (with_other_conjuncts) {
                        _same_to_prev.emplace_back(false);
                        for (int i = 0; i < current_offset - origin_offset - 1; ++i) {
                            _same_to_prev.emplace_back(true);
                        }
                    }
                } else if constexpr (probe_all || JoinOpType == TJoinOp::LEFT_ANTI_JOIN ||
                                     JoinOpType == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN ||
                                     (JoinOpType == TJoinOp::LEFT_SEMI_JOIN && is_mark_join)) {
                    // only full outer / left outer need insert the data of right table
                    _emplace_element(-1, -1, current_offset);

                    if constexpr (with_other_conjuncts) {
                        _same_to_prev.emplace_back(false);
                        _visited_map.emplace_back(nullptr);
                    }
                    ++probe_index;
                } else {
                    ++probe_index;
                }
            }
            all_match_one &= (current_offset == _probe_indexs.size() + 1);
            _probe_indexs.resize(current_offset, current_probe_index);
        }
        probe_size = probe_index - last_probe_index + probe_row_match_iter.ok();
    }

    build_side_output_column(mcol, *_join_context->_right_output_slot_flags, current_offset,
                             with_other_conjuncts);

    if constexpr (with_other_conjuncts || (JoinOpType != TJoinOp::RIGHT_SEMI_JOIN &&
                                           JoinOpType != TJoinOp::RIGHT_ANTI_JOIN)) {
        RETURN_IF_CATCH_EXCEPTION(probe_side_output_column(
                mcol, *_join_context->_left_output_slot_flags, current_offset, last_probe_index,
                probe_size, all_match_one, with_other_conjuncts));
    }

    output_block->swap(mutable_block.to_block());

    if constexpr (with_other_conjuncts) {
        return do_other_join_conjuncts(output_block, is_mark_join, multi_matched_output_row_count,
                                       is_the_last_sub_block);
    }

    return Status::OK();
}

template <int JoinOpType>
Status ProcessHashTableProbe<JoinOpType>::do_other_join_conjuncts(
        Block* output_block, bool is_mark_join, int multi_matched_output_row_count,
        bool is_the_last_sub_block) {
    // dispose the other join conjunct exec
    auto row_count = output_block->rows();
    if (!row_count) {
        return Status::OK();
    }

    SCOPED_TIMER(_join_context->_process_other_join_conjunct_timer);
    int orig_columns = output_block->columns();
    IColumn::Filter other_conjunct_filter(row_count, 1);
    {
        bool can_be_filter_all = false;
        RETURN_IF_ERROR(VExprContext::execute_conjuncts(
                *_join_context->_other_join_conjuncts, nullptr, output_block,
                &other_conjunct_filter, &can_be_filter_all));
    }

    auto filter_column = ColumnUInt8::create();
    filter_column->get_data() = std::move(other_conjunct_filter);
    auto result_column_id = output_block->columns();
    output_block->insert({std::move(filter_column), std::make_shared<DataTypeUInt8>(), ""});
    const uint8_t* __restrict filter_column_ptr =
            assert_cast<const ColumnUInt8*>(
                    output_block->get_by_position(result_column_id).column.get())
                    ->get_data()
                    .data();

    if constexpr (JoinOpType == TJoinOp::LEFT_OUTER_JOIN ||
                  JoinOpType == TJoinOp::FULL_OUTER_JOIN) {
        auto new_filter_column = ColumnVector<UInt8>::create(row_count);
        auto* __restrict filter_map = new_filter_column->get_data().data();

        auto null_map_column = ColumnVector<UInt8>::create(row_count, 0);
        auto* __restrict null_map_data = null_map_column->get_data().data();

        // It contains non-first sub block of splited equal-conjuncts-matched tuples from last probe row
        if (_row_count_from_last_probe > 0) {
            _process_splited_equal_matched_tuples(0, _row_count_from_last_probe, filter_column_ptr,
                                                  null_map_data, filter_map, output_block);
            // This is the last sub block of splitted block, and no equal-conjuncts-matched tuple
            // is output in all sub blocks, need to output a tuple for this probe row
            if (is_the_last_sub_block && !*_join_context->_is_any_probe_match_row_output) {
                filter_map[0] = true;
                null_map_data[0] = true;
            }
        }
        int end_idx = row_count - multi_matched_output_row_count;
        // process equal-conjuncts-matched tuples that are newly generated
        // in this run if there are any.
        for (int i = _row_count_from_last_probe; i < end_idx; ++i) {
            auto join_hit = _visited_map[i] != nullptr;
            auto other_hit = filter_column_ptr[i];

            if (!other_hit) {
                for (size_t j = 0; j < _right_col_len; ++j) {
                    typeid_cast<ColumnNullable*>(
                            std::move(*output_block->get_by_position(j + _right_col_idx).column)
                                    .assume_mutable()
                                    .get())
                            ->get_null_map_data()[i] = true;
                }
            }
            null_map_data[i] = !join_hit || !other_hit;

            // For cases where one probe row matches multiple build rows for equal conjuncts,
            // all the other-conjuncts-matched tuples should be output.
            //
            // Other-conjuncts-NOT-matched tuples fall into two categories:
            //    1. The beginning consecutive one(s).
            //       For these tuples, only the last one is marked to output;
            //       If there are any following other-conjuncts-matched tuples,
            //       the last tuple is also marked NOT to output.
            //    2. All the remaining other-conjuncts-NOT-matched tuples.
            //       All these tuples are marked not to output.
            if (join_hit) {
                *_visited_map[i] |= other_hit;
                filter_map[i] = other_hit || !_same_to_prev[i] ||
                                (!filter_column_ptr[i] && filter_map[i - 1]);
                // Here to keep only hit join conjunct and other join conjunt is true need to be output.
                // if not, only some key must keep one row will output will null right table column
                if (_same_to_prev[i] && filter_map[i] && !filter_column_ptr[i - 1]) {
                    filter_map[i - 1] = false;
                }
            } else {
                filter_map[i] = true;
            }
        }

        // It contains the first sub block of splited equal-conjuncts-matched tuples of the current probe row
        if (multi_matched_output_row_count > 0) {
            *_join_context->_is_any_probe_match_row_output = false;
            _process_splited_equal_matched_tuples(row_count - multi_matched_output_row_count,
                                                  multi_matched_output_row_count, filter_column_ptr,
                                                  null_map_data, filter_map, output_block);
        }

        for (size_t i = 0; i < row_count; ++i) {
            if (filter_map[i]) {
                _tuple_is_null_right_flags->emplace_back(null_map_data[i]);
            }
        }
        output_block->get_by_position(result_column_id).column = std::move(new_filter_column);
    } else if constexpr (JoinOpType == TJoinOp::LEFT_SEMI_JOIN) {
        // TODO: resize in advance
        auto new_filter_column = ColumnVector<UInt8>::create();
        auto& filter_map = new_filter_column->get_data();

        size_t start_row_idx = 1;
        // We are handling euqual-conjuncts matched tuples that are splitted into multiple blocks
        if (_row_count_from_last_probe > 0) {
            if (*_join_context->_is_any_probe_match_row_output) {
                // if any matched tuple for this probe row is output,
                // ignore all the following tuples for this probe row.
                for (int row_idx = 0; row_idx < _row_count_from_last_probe; ++row_idx) {
                    filter_map.emplace_back(false);
                }
                start_row_idx += _row_count_from_last_probe;
                if (_row_count_from_last_probe < row_count) {
                    filter_map.emplace_back(filter_column_ptr[_row_count_from_last_probe]);
                }
            } else {
                filter_map.emplace_back(filter_column_ptr[0]);
            }
        } else {
            filter_map.emplace_back(filter_column_ptr[0]);
        }
        for (size_t i = start_row_idx; i < row_count; ++i) {
            if (filter_column_ptr[i] || (_same_to_prev[i] && filter_map[i - 1])) {
                // Only last same element is true, output last one
                filter_map.push_back(true);
                filter_map[i - 1] = !_same_to_prev[i] && filter_map[i - 1];
            } else {
                filter_map.push_back(false);
            }
        }
        // It contains the first sub block of splited equal-conjuncts-matched tuples of the current probe row
        if (multi_matched_output_row_count > 0) {
            // If a matched row is output, all the equal-matched tuples in
            // the following sub blocks should be ignored
            *_join_context->_is_any_probe_match_row_output = filter_map[row_count - 1];
        } else if (_row_count_from_last_probe > 0 &&
                   !*_join_context->_is_any_probe_match_row_output) {
            // We are handling euqual-conjuncts matched tuples that are splitted into multiple blocks,
            // and no matched tuple has been output in all previous run.
            // If a tuple is output in this run, all the following mathced tuples should be ignored
            if (filter_map[_row_count_from_last_probe - 1]) {
                *_join_context->_is_any_probe_match_row_output = true;
            }
        }

        if (is_mark_join) {
            auto& matched_map = assert_cast<ColumnVector<UInt8>&>(
                                        *(output_block->get_by_position(orig_columns - 1)
                                                  .column->assume_mutable()))
                                        .get_data();

            // For mark join, we only filter rows which have duplicate join keys.
            // And then, we set matched_map to the join result to do the mark join's filtering.
            for (size_t i = 1; i < row_count; ++i) {
                if (!_same_to_prev[i]) {
                    matched_map.push_back(filter_map[i - 1]);
                    filter_map[i - 1] = true;
                }
            }
            matched_map.push_back(filter_map[filter_map.size() - 1]);
            filter_map[filter_map.size() - 1] = true;
        }

        output_block->get_by_position(result_column_id).column = std::move(new_filter_column);
    } else if constexpr (JoinOpType == TJoinOp::LEFT_ANTI_JOIN ||
                         JoinOpType == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) {
        auto new_filter_column = ColumnVector<UInt8>::create(row_count);
        auto* __restrict filter_map = new_filter_column->get_data().data();

        // for left anti join, the probe side is output only when
        // there are no matched tuples for the probe row.

        // If multiple equal-conjuncts-matched tuples is splitted into several
        // sub blocks, just filter out all the other-conjuncts-NOT-matched tuples at first,
        // and when processing the last sub block, check whether there are any
        // equal-conjuncts-matched tuple is output in all sub blocks,
        // if there are none, just pick a tuple and output.

        size_t start_row_idx = 1;
        // We are handling euqual-conjuncts matched tuples that are splitted into multiple blocks
        if (_row_count_from_last_probe > 0 && *_join_context->_is_any_probe_match_row_output) {
            // if any matched tuple for this probe row is output,
            // ignore all the following tuples for this probe row.
            for (int row_idx = 0; row_idx < _row_count_from_last_probe; ++row_idx) {
                filter_map[row_idx] = false;
            }
            start_row_idx += _row_count_from_last_probe;
            if (_row_count_from_last_probe < row_count) {
                filter_map[_row_count_from_last_probe] =
                        filter_column_ptr[_row_count_from_last_probe] &&
                        _visited_map[_row_count_from_last_probe];
            }
        } else {
            // Both equal conjuncts and other conjuncts are true
            filter_map[0] = filter_column_ptr[0] && _visited_map[0];
        }

        for (size_t i = start_row_idx; i < row_count; ++i) {
            if ((_visited_map[i] && filter_column_ptr[i]) ||
                (_same_to_prev[i] && filter_map[i - 1])) {
                // When either of two conditions is meet:
                // 1. Both equal conjuncts and other conjuncts are true or same_to_prev
                // 2. This row is joined from the same build side row as the previous row
                // Set filter_map[i] to true and filter_map[i - 1] to false if same_to_prev[i]
                // is true.
                filter_map[i] = true;
                filter_map[i - 1] = !_same_to_prev[i] && filter_map[i - 1];
            } else {
                filter_map[i] = false;
            }
        }

        if (is_mark_join) {
            auto& matched_map = assert_cast<ColumnVector<UInt8>&>(
                                        *(output_block->get_by_position(orig_columns - 1)
                                                  .column->assume_mutable()))
                                        .get_data();
            for (int i = 1; i < row_count; ++i) {
                if (!_same_to_prev[i]) {
                    matched_map.push_back(!filter_map[i - 1]);
                    filter_map[i - 1] = true;
                }
            }
            matched_map.push_back(!filter_map[row_count - 1]);
            filter_map[row_count - 1] = true;
        } else {
            int end_row_idx = 0;
            if (_row_count_from_last_probe > 0) {
                end_row_idx = row_count - multi_matched_output_row_count;
                if (!*_join_context->_is_any_probe_match_row_output) {
                    // We are handling euqual-conjuncts matched tuples that are splitted into multiple blocks,
                    // and no matched tuple has been output in all previous run.
                    // If a tuple is output in this run, all the following mathced tuples should be ignored
                    if (filter_map[_row_count_from_last_probe - 1]) {
                        *_join_context->_is_any_probe_match_row_output = true;
                        filter_map[_row_count_from_last_probe - 1] = false;
                    }
                    if (is_the_last_sub_block && !*_join_context->_is_any_probe_match_row_output) {
                        // This is the last sub block of splitted block, and no equal-conjuncts-matched tuple
                        // is output in all sub blocks, output a tuple for this probe row
                        filter_map[0] = true;
                    }
                }
                if (multi_matched_output_row_count > 0) {
                    // It contains the first sub block of splited equal-conjuncts-matched tuples of the current probe row
                    // If a matched row is output, all the equal-matched tuples in
                    // the following sub blocks should be ignored
                    *_join_context->_is_any_probe_match_row_output = filter_map[row_count - 1];
                    filter_map[row_count - 1] = false;
                }
            } else if (multi_matched_output_row_count > 0) {
                end_row_idx = row_count - multi_matched_output_row_count;
                // It contains the first sub block of splited equal-conjuncts-matched tuples of the current probe row
                // If a matched row is output, all the equal-matched tuples in
                // the following sub blocks should be ignored
                *_join_context->_is_any_probe_match_row_output = filter_map[row_count - 1];
                filter_map[row_count - 1] = false;
            } else {
                end_row_idx = row_count;
            }

            // Same to the semi join, but change the last value to opposite value
            for (int i = 1 + _row_count_from_last_probe; i < end_row_idx; ++i) {
                if (!_same_to_prev[i]) {
                    filter_map[i - 1] = !filter_map[i - 1];
                }
            }
            auto non_sub_blocks_matched_row_count =
                    row_count - _row_count_from_last_probe - multi_matched_output_row_count;
            if (non_sub_blocks_matched_row_count > 0) {
                filter_map[end_row_idx - 1] = !filter_map[end_row_idx - 1];
            }
        }

        output_block->get_by_position(result_column_id).column = std::move(new_filter_column);
    } else if constexpr (JoinOpType == TJoinOp::RIGHT_SEMI_JOIN ||
                         JoinOpType == TJoinOp::RIGHT_ANTI_JOIN) {
        for (int i = 0; i < row_count; ++i) {
            DCHECK(_visited_map[i]);
            *_visited_map[i] |= filter_column_ptr[i];
        }
    } else if constexpr (JoinOpType == TJoinOp::RIGHT_OUTER_JOIN) {
        auto filter_size = 0;
        for (int i = 0; i < row_count; ++i) {
            DCHECK(_visited_map[i]);
            auto result = filter_column_ptr[i];
            *_visited_map[i] |= result;
            filter_size += result;
        }
        _tuple_is_null_left_flags->resize_fill(filter_size, 0);
    }

    if constexpr (JoinOpType == TJoinOp::RIGHT_SEMI_JOIN ||
                  JoinOpType == TJoinOp::RIGHT_ANTI_JOIN) {
        output_block->clear();
    } else {
        if constexpr (JoinOpType == TJoinOp::LEFT_SEMI_JOIN ||
                      JoinOpType == TJoinOp::LEFT_ANTI_JOIN ||
                      JoinOpType == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) {
            orig_columns = _right_col_idx;
        }
        Block::filter_block(output_block, result_column_id,
                            is_mark_join ? output_block->columns() : orig_columns);
    }

    return Status::OK();
}

// For left or full outer join with other conjuncts.
// If multiple equal-conjuncts-matched tuples is splitted into several
// sub blocks, just filter out all the other-conjuncts-NOT-matched tuples at first,
// and when processing the last sub block, check whether there are any
// equal-conjuncts-matched tuple is output in all sub blocks,
// if not, just pick a tuple and output.
template <int JoinOpType>
void ProcessHashTableProbe<JoinOpType>::_process_splited_equal_matched_tuples(
        int start_row_idx, int row_count, const UInt8* __restrict other_hit_column,
        UInt8* __restrict null_map_data, UInt8* __restrict filter_map, Block* output_block) {
    int end_row_idx = start_row_idx + row_count;
    for (int i = start_row_idx; i < end_row_idx; ++i) {
        auto join_hit = _visited_map[i] != nullptr;
        auto other_hit = other_hit_column[i];

        if (!other_hit) {
            for (size_t j = 0; j < _right_col_len; ++j) {
                typeid_cast<ColumnNullable*>(
                        std::move(*output_block->get_by_position(j + _right_col_idx).column)
                                .assume_mutable()
                                .get())
                        ->get_null_map_data()[i] = true;
            }
        }

        null_map_data[i] = !join_hit || !other_hit;
        filter_map[i] = other_hit;

        if (join_hit) {
            *_visited_map[i] |= other_hit;
        }
    }
    *_join_context->_is_any_probe_match_row_output |=
            simd::contain_byte(filter_map + start_row_idx, row_count, 1);
}

template <int JoinOpType>
template <typename HashTableType>
Status ProcessHashTableProbe<JoinOpType>::process_data_in_hashtable(HashTableType& hash_table_ctx,
                                                                    MutableBlock& mutable_block,
                                                                    Block* output_block,
                                                                    bool* eos) {
    using Mapped = typename HashTableType::Mapped;
    SCOPED_TIMER(_probe_process_hashtable_timer);
    if constexpr (std::is_same_v<Mapped, RowRefListWithFlag> ||
                  std::is_same_v<Mapped, RowRefListWithFlags>) {
        hash_table_ctx.init_once();
        auto& mcol = mutable_block.mutable_columns();

        bool right_semi_anti_without_other =
                _join_context->_is_right_semi_anti && !_join_context->_have_other_join_conjunct;
        int right_col_idx =
                right_semi_anti_without_other ? 0 : _join_context->_left_table_data_types->size();
        int right_col_len = _join_context->_right_table_data_types->size();

        auto& iter = hash_table_ctx.iter;
        auto block_size = 0;
        auto& visited_iter =
                std::get<ForwardIterator<Mapped>>(*_join_context->_outer_join_pull_visited_iter);
        _build_blocks_locs.resize(_batch_size);
        auto register_build_loc = [&](int8_t offset, int32_t row_nums) {
            _build_blocks_locs[block_size++] = std::pair<int8_t, int>(offset, row_nums);
        };

        if (visited_iter.ok()) {
            if constexpr (std::is_same_v<Mapped, RowRefListWithFlag>) {
                for (; visited_iter.ok() && block_size < _batch_size; ++visited_iter) {
                    register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                }
            } else {
                for (; visited_iter.ok() && block_size < _batch_size; ++visited_iter) {
                    if constexpr (JoinOpType == TJoinOp::RIGHT_SEMI_JOIN) {
                        if (visited_iter->visited) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                    } else {
                        if (!visited_iter->visited) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                    }
                }
            }
            if (!visited_iter.ok()) {
                ++iter;
            }
        }

        for (; iter != hash_table_ctx.hash_table.end() && block_size < _batch_size; ++iter) {
            auto& mapped = iter->get_second();
            if constexpr (std::is_same_v<Mapped, RowRefListWithFlag>) {
                if (mapped.visited) {
                    if constexpr (JoinOpType == TJoinOp::RIGHT_SEMI_JOIN) {
                        visited_iter = mapped.begin();
                        for (; visited_iter.ok() && block_size < _batch_size; ++visited_iter) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                        if (visited_iter.ok()) {
                            // block_size >= _batch_size, quit for loop
                            break;
                        }
                    }
                } else {
                    if constexpr (JoinOpType != TJoinOp::RIGHT_SEMI_JOIN) {
                        visited_iter = mapped.begin();
                        for (; visited_iter.ok() && block_size < _batch_size; ++visited_iter) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                        if (visited_iter.ok()) {
                            // block_size >= _batch_size, quit for loop
                            break;
                        }
                    }
                }
            } else {
                visited_iter = mapped.begin();
                for (; visited_iter.ok() && block_size < _batch_size; ++visited_iter) {
                    if constexpr (JoinOpType == TJoinOp::RIGHT_SEMI_JOIN) {
                        if (visited_iter->visited) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                    } else {
                        if (!visited_iter->visited) {
                            register_build_loc(visited_iter->block_offset, visited_iter->row_num);
                        }
                    }
                }
                if (visited_iter.ok()) {
                    // block_size >= _batch_size, quit for loop
                    break;
                }
            }
        }
        _build_blocks_locs.resize(block_size);

        auto insert_build_rows = [&](int8_t offset) {
            for (size_t j = 0; j < right_col_len; ++j) {
                auto& column = *_build_blocks[offset].get_by_position(j).column;
                mcol[j + right_col_idx]->insert_indices_from(
                        column, _build_block_rows.data(),
                        _build_block_rows.data() + _build_block_rows.size());
            }
        };
        if (_build_blocks.size() > 1) {
            std::sort(_build_blocks_locs.begin(), _build_blocks_locs.end(),
                      [](const auto a, const auto b) { return a.first > b.first; });
            auto start = 0, end = 0;
            while (start < _build_blocks_locs.size()) {
                while (end < _build_blocks_locs.size() &&
                       _build_blocks_locs[start].first == _build_blocks_locs[end].first) {
                    end++;
                }
                auto offset = _build_blocks_locs[start].first;
                _build_block_rows.resize(end - start);
                for (int i = 0; start + i < end; i++) {
                    _build_block_rows[i] = _build_blocks_locs[start + i].second;
                }
                start = end;
                insert_build_rows(offset);
            }
        } else if (_build_blocks.size() == 1) {
            const auto size = _build_blocks_locs.size();
            _build_block_rows.resize(_build_blocks_locs.size());
            for (int i = 0; i < size; i++) {
                _build_block_rows[i] = _build_blocks_locs[i].second;
            }
            insert_build_rows(0);
        }

        // just resize the left table column in case with other conjunct to make block size is not zero
        if (_join_context->_is_right_semi_anti && _join_context->_have_other_join_conjunct) {
            auto target_size = mcol[right_col_idx]->size();
            for (int i = 0; i < right_col_idx; ++i) {
                mcol[i]->resize(target_size);
            }
        }

        // right outer join / full join need insert data of left table
        if constexpr (JoinOpType == TJoinOp::RIGHT_OUTER_JOIN ||
                      JoinOpType == TJoinOp::FULL_OUTER_JOIN) {
            for (int i = 0; i < right_col_idx; ++i) {
                assert_cast<ColumnNullable*>(mcol[i].get())->insert_many_defaults(block_size);
            }
            _tuple_is_null_left_flags->resize_fill(block_size, 1);
        }
        *eos = iter == hash_table_ctx.hash_table.end();
        output_block->swap(
                mutable_block.to_block(right_semi_anti_without_other ? right_col_idx : 0));
        DCHECK(block_size <= _batch_size);
        return Status::OK();
    } else {
        LOG(FATAL) << "Invalid RowRefList";
        return Status::InvalidArgument("Invalid RowRefList");
    }
}

template <int JoinOpType>
template <bool need_null_map_for_probe, bool ignore_null, typename HashTableType>
Status ProcessHashTableProbe<JoinOpType>::process(HashTableType& hash_table_ctx,
                                                  ConstNullMapPtr null_map,
                                                  MutableBlock& mutable_block, Block* output_block,
                                                  size_t probe_rows, bool is_mark_join,
                                                  bool have_other_join_conjunct) {
    Status res;
    if constexpr (!std::is_same_v<typename HashTableType::Mapped, RowRefListWithFlags>) {
        if (have_other_join_conjunct) {
            res = Status::InvalidArgument("Invalid HashTableType::Mapped");
        } else {
            std::visit(
                    [&](auto is_mark_join) {
                        res = do_process<need_null_map_for_probe, ignore_null, HashTableType, false,
                                         is_mark_join>(hash_table_ctx, null_map, mutable_block,
                                                       output_block, probe_rows);
                    },
                    make_bool_variant(is_mark_join));
        }
    } else {
        std::visit(
                [&](auto is_mark_join, auto have_other_join_conjunct) {
                    res = do_process<need_null_map_for_probe, ignore_null, HashTableType,
                                     have_other_join_conjunct, is_mark_join>(
                            hash_table_ctx, null_map, mutable_block, output_block, probe_rows);
                },
                make_bool_variant(is_mark_join), make_bool_variant(have_other_join_conjunct));
    }
    return res;
}

template <typename T>
struct ExtractType;

template <typename T, typename U>
struct ExtractType<T(U)> {
    using Type = U;
};

#define INSTANTIATION(JoinOpType, T)                                                          \
    template Status                                                                           \
    ProcessHashTableProbe<JoinOpType>::process<false, false, ExtractType<void(T)>::Type>(     \
            ExtractType<void(T)>::Type & hash_table_ctx, ConstNullMapPtr null_map,            \
            MutableBlock & mutable_block, Block * output_block, size_t probe_rows,            \
            bool is_mark_join, bool have_other_join_conjunct);                                \
    template Status                                                                           \
    ProcessHashTableProbe<JoinOpType>::process<false, true, ExtractType<void(T)>::Type>(      \
            ExtractType<void(T)>::Type & hash_table_ctx, ConstNullMapPtr null_map,            \
            MutableBlock & mutable_block, Block * output_block, size_t probe_rows,            \
            bool is_mark_join, bool have_other_join_conjunct);                                \
    template Status                                                                           \
    ProcessHashTableProbe<JoinOpType>::process<true, false, ExtractType<void(T)>::Type>(      \
            ExtractType<void(T)>::Type & hash_table_ctx, ConstNullMapPtr null_map,            \
            MutableBlock & mutable_block, Block * output_block, size_t probe_rows,            \
            bool is_mark_join, bool have_other_join_conjunct);                                \
    template Status                                                                           \
    ProcessHashTableProbe<JoinOpType>::process<true, true, ExtractType<void(T)>::Type>(       \
            ExtractType<void(T)>::Type & hash_table_ctx, ConstNullMapPtr null_map,            \
            MutableBlock & mutable_block, Block * output_block, size_t probe_rows,            \
            bool is_mark_join, bool have_other_join_conjunct);                                \
                                                                                              \
    template Status                                                                           \
    ProcessHashTableProbe<JoinOpType>::process_data_in_hashtable<ExtractType<void(T)>::Type>( \
            ExtractType<void(T)>::Type & hash_table_ctx, MutableBlock & mutable_block,        \
            Block * output_block, bool* eos)

#define INSTANTIATION_FOR(JoinOpType)                                                      \
    template struct ProcessHashTableProbe<JoinOpType>;                                     \
                                                                                           \
    INSTANTIATION(JoinOpType, (SerializedHashTableContext<RowRefList>));                   \
    INSTANTIATION(JoinOpType, (I8HashTableContext<RowRefList>));                           \
    INSTANTIATION(JoinOpType, (I16HashTableContext<RowRefList>));                          \
    INSTANTIATION(JoinOpType, (I32HashTableContext<RowRefList>));                          \
    INSTANTIATION(JoinOpType, (I64HashTableContext<RowRefList>));                          \
    INSTANTIATION(JoinOpType, (I128HashTableContext<RowRefList>));                         \
    INSTANTIATION(JoinOpType, (I256HashTableContext<RowRefList>));                         \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<true, RowRefList>));            \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<false, RowRefList>));           \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<true, RowRefList>));           \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<false, RowRefList>));          \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<true, RowRefList>));           \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<false, RowRefList>));          \
    INSTANTIATION(JoinOpType, (SerializedHashTableContext<RowRefListWithFlag>));           \
    INSTANTIATION(JoinOpType, (I8HashTableContext<RowRefListWithFlag>));                   \
    INSTANTIATION(JoinOpType, (I16HashTableContext<RowRefListWithFlag>));                  \
    INSTANTIATION(JoinOpType, (I32HashTableContext<RowRefListWithFlag>));                  \
    INSTANTIATION(JoinOpType, (I64HashTableContext<RowRefListWithFlag>));                  \
    INSTANTIATION(JoinOpType, (I128HashTableContext<RowRefListWithFlag>));                 \
    INSTANTIATION(JoinOpType, (I256HashTableContext<RowRefListWithFlag>));                 \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<true, RowRefListWithFlag>));    \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<false, RowRefListWithFlag>));   \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<true, RowRefListWithFlag>));   \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<false, RowRefListWithFlag>));  \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<true, RowRefListWithFlag>));   \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<false, RowRefListWithFlag>));  \
    INSTANTIATION(JoinOpType, (SerializedHashTableContext<RowRefListWithFlags>));          \
    INSTANTIATION(JoinOpType, (I8HashTableContext<RowRefListWithFlags>));                  \
    INSTANTIATION(JoinOpType, (I16HashTableContext<RowRefListWithFlags>));                 \
    INSTANTIATION(JoinOpType, (I32HashTableContext<RowRefListWithFlags>));                 \
    INSTANTIATION(JoinOpType, (I64HashTableContext<RowRefListWithFlags>));                 \
    INSTANTIATION(JoinOpType, (I128HashTableContext<RowRefListWithFlags>));                \
    INSTANTIATION(JoinOpType, (I256HashTableContext<RowRefListWithFlags>));                \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<true, RowRefListWithFlags>));   \
    INSTANTIATION(JoinOpType, (I64FixedKeyHashTableContext<false, RowRefListWithFlags>));  \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<true, RowRefListWithFlags>));  \
    INSTANTIATION(JoinOpType, (I128FixedKeyHashTableContext<false, RowRefListWithFlags>)); \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<true, RowRefListWithFlags>));  \
    INSTANTIATION(JoinOpType, (I256FixedKeyHashTableContext<false, RowRefListWithFlags>))

} // namespace doris::vectorized
