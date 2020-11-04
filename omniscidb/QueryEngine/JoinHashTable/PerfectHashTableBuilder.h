/*
 * Copyright 2020 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "QueryEngine/JoinHashTable/PerfectHashTable.h"

#include "Shared/scope.h"

class PerfectJoinHashTableBuilder {
 public:
  PerfectJoinHashTableBuilder(const Catalog_Namespace::Catalog* catalog)
      : hash_table_(std::make_unique<PerfectHashTable>(catalog)) {}

  void allocateDeviceMemory(const JoinColumn& join_column,
                            const JoinHashTableInterface::HashType layout,
                            HashEntryInfo& hash_entry_info,
                            const size_t shard_count,
                            const int device_id,
                            const int device_count) {
#ifdef HAVE_CUDA
    if (shard_count) {
      const auto shards_per_device = (shard_count + device_count - 1) / device_count;
      CHECK_GT(shards_per_device, 0u);
      const size_t entries_per_shard =
          get_entries_per_shard(hash_entry_info.hash_entry_count, shard_count);
      hash_entry_info.hash_entry_count = entries_per_shard * shards_per_device;
    }
    const size_t total_count =
        layout == JoinHashTableInterface::HashType::OneToOne
            ? hash_entry_info.getNormalizedHashEntryCount()
            : 2 * hash_entry_info.getNormalizedHashEntryCount() + join_column.num_elems;
    CHECK(hash_table_);
    hash_table_->allocateGpuMemory(total_count, device_id);
#else
    UNREACHABLE();
#endif  // HAVE_CUDA
  }

#ifdef HAVE_CUDA
  void initHashTableOnGpu(const ChunkKey& chunk_key,
                          const JoinColumn& join_column,
                          const ExpressionRange& col_range,
                          const bool is_bitwise_eq,
                          const InnerOuter& cols,
                          const JoinHashTableInterface::HashType layout,
                          const HashEntryInfo hash_entry_info,
                          const size_t shard_count,
                          const int32_t hash_join_invalid_val,
                          const int device_id,
                          const int device_count,
                          const Executor* executor) {
    auto catalog = executor->getCatalog();
    auto& data_mgr = catalog->getDataMgr();
    Data_Namespace::AbstractBuffer* gpu_hash_table_err_buff =
        CudaAllocator::allocGpuAbstractBuffer(&data_mgr, sizeof(int), device_id);
    ScopeGuard cleanup_error_buff = [&data_mgr, gpu_hash_table_err_buff]() {
      data_mgr.free(gpu_hash_table_err_buff);
    };
    CHECK(gpu_hash_table_err_buff);
    auto dev_err_buff =
        reinterpret_cast<CUdeviceptr>(gpu_hash_table_err_buff->getMemoryPtr());
    int err{0};
    copy_to_gpu(&data_mgr, dev_err_buff, &err, sizeof(err), device_id);

    CHECK(hash_table_);
    auto gpu_hash_table_buff = hash_table_->gpuBufferPtr();

    init_hash_join_buff_on_device(reinterpret_cast<int32_t*>(gpu_hash_table_buff),
                                  hash_entry_info.getNormalizedHashEntryCount(),
                                  hash_join_invalid_val,
                                  executor->blockSize(),
                                  executor->gridSize());
    if (chunk_key.empty()) {
      return;  // TODO(?) maybe this is ok since we init an empty table?
    }

    // TODO: maybe was pass this in? duplicated in JoinHashTable currently
    const auto inner_col = cols.first;
    CHECK(inner_col);
    const auto& ti = inner_col->get_type_info();

    JoinColumnTypeInfo type_info{static_cast<size_t>(ti.get_size()),
                                 col_range.getIntMin(),
                                 col_range.getIntMax(),
                                 inline_fixed_encoding_null_val(ti),
                                 is_bitwise_eq,
                                 col_range.getIntMax() + 1,
                                 get_join_column_type_kind(ti)};
    auto use_bucketization = inner_col->get_type_info().get_type() == kDATE;
    if (shard_count) {
      const size_t entries_per_shard =
          get_entries_per_shard(hash_entry_info.hash_entry_count, shard_count);
      CHECK_GT(device_count, 0);
      for (size_t shard = device_id; shard < shard_count; shard += device_count) {
        ShardInfo shard_info{shard, entries_per_shard, shard_count, device_count};
        if (layout == JoinHashTableInterface::HashType::OneToOne) {
          fill_hash_join_buff_on_device_sharded_bucketized(
              reinterpret_cast<int32_t*>(gpu_hash_table_buff),
              hash_join_invalid_val,
              reinterpret_cast<int*>(dev_err_buff),
              join_column,
              type_info,
              shard_info,
              executor->blockSize(),
              executor->gridSize(),
              hash_entry_info.bucket_normalization);
        } else {
          fill_one_to_many_hash_table_on_device_sharded(
              reinterpret_cast<int32_t*>(gpu_hash_table_buff),
              hash_entry_info,
              hash_join_invalid_val,
              join_column,
              type_info,
              shard_info,
              executor->blockSize(),
              executor->gridSize());
        }
      }
    } else {
      if (layout == JoinHashTableInterface::HashType::OneToOne) {
        fill_hash_join_buff_on_device_bucketized(
            reinterpret_cast<int32_t*>(gpu_hash_table_buff),
            hash_join_invalid_val,
            reinterpret_cast<int*>(dev_err_buff),
            join_column,
            type_info,
            executor->blockSize(),
            executor->gridSize(),
            hash_entry_info.bucket_normalization);
      } else {
        if (use_bucketization) {
          fill_one_to_many_hash_table_on_device_bucketized(
              reinterpret_cast<int32_t*>(gpu_hash_table_buff),
              hash_entry_info,
              hash_join_invalid_val,
              join_column,
              type_info,
              executor->blockSize(),
              executor->gridSize());
        } else {
          fill_one_to_many_hash_table_on_device(
              reinterpret_cast<int32_t*>(gpu_hash_table_buff),
              hash_entry_info,
              hash_join_invalid_val,
              join_column,
              type_info,
              executor->blockSize(),
              executor->gridSize());
        }
      }
    }
    copy_from_gpu(&data_mgr, &err, dev_err_buff, sizeof(err), device_id);
    if (err) {
      if (layout == JoinHashTableInterface::HashType::OneToOne) {
        throw NeedsOneToManyHash();
      } else {
        throw std::runtime_error("Unexpected error when building perfect hash table: " +
                                 std::to_string(err));
      }
    }
  }
#endif

  void initOneToOneHashTableOnCpu(const JoinColumn& join_column,
                                  const ExpressionRange& col_range,
                                  const bool is_bitwise_eq,
                                  const InnerOuter& cols,
                                  const HashEntryInfo hash_entry_info,
                                  const int32_t hash_join_invalid_val,
                                  const Executor* executor) {
    auto timer = DEBUG_TIMER(__func__);
    const auto inner_col = cols.first;
    CHECK(inner_col);
    const auto& ti = inner_col->get_type_info();

    auto cpu_hash_table_buff = std::make_shared<std::vector<int32_t>>(
        hash_entry_info.getNormalizedHashEntryCount());
    const StringDictionaryProxy* sd_inner_proxy{nullptr};
    const StringDictionaryProxy* sd_outer_proxy{nullptr};
    const auto outer_col = dynamic_cast<const Analyzer::ColumnVar*>(cols.second);
    if (ti.is_string() &&
        (outer_col && !(inner_col->get_comp_param() == outer_col->get_comp_param()))) {
      CHECK_EQ(kENCODING_DICT, ti.get_compression());
      sd_inner_proxy =
          executor->getStringDictionaryProxy(inner_col->get_comp_param(), true);
      CHECK(sd_inner_proxy);
      CHECK(outer_col);
      sd_outer_proxy =
          executor->getStringDictionaryProxy(outer_col->get_comp_param(), true);
      CHECK(sd_outer_proxy);
    }
    int thread_count = cpu_threads();
    std::vector<std::thread> init_cpu_buff_threads;
    for (int thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
      init_cpu_buff_threads.emplace_back([this,
                                          hash_entry_info,
                                          hash_join_invalid_val,
                                          thread_idx,
                                          thread_count,
                                          &cpu_hash_table_buff] {
        init_hash_join_buff(cpu_hash_table_buff->data(),
                            hash_entry_info.getNormalizedHashEntryCount(),
                            hash_join_invalid_val,
                            thread_idx,
                            thread_count);
      });
    }
    for (auto& t : init_cpu_buff_threads) {
      t.join();
    }
    init_cpu_buff_threads.clear();
    std::atomic<int> err{0};
    for (int thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
      init_cpu_buff_threads.emplace_back([this,
                                          hash_join_invalid_val,
                                          &join_column,
                                          sd_inner_proxy,
                                          sd_outer_proxy,
                                          thread_idx,
                                          thread_count,
                                          &ti,
                                          &err,
                                          &col_range,
                                          &is_bitwise_eq,
                                          &cpu_hash_table_buff,
                                          hash_entry_info] {
        int partial_err =
            fill_hash_join_buff_bucketized(cpu_hash_table_buff->data(),
                                           hash_join_invalid_val,
                                           join_column,
                                           {static_cast<size_t>(ti.get_size()),
                                            col_range.getIntMin(),
                                            col_range.getIntMax(),
                                            inline_fixed_encoding_null_val(ti),
                                            is_bitwise_eq,
                                            col_range.getIntMax() + 1,
                                            get_join_column_type_kind(ti)},
                                           sd_inner_proxy,
                                           sd_outer_proxy,
                                           thread_idx,
                                           thread_count,
                                           hash_entry_info.bucket_normalization);
        int zero{0};
        err.compare_exchange_strong(zero, partial_err);
      });
    }
    for (auto& t : init_cpu_buff_threads) {
      t.join();
    }
    if (err) {
      // cpu_hash_table_buff.data().reset();
      // Too many hash entries, need to retry with a 1:many table
      throw NeedsOneToManyHash();
    }
    CHECK(hash_table_);
    hash_table_->setHashTableBuffer(cpu_hash_table_buff);
  }

  void initOneToManyHashTableOnCpu(
      const JoinColumn& join_column,
      const ExpressionRange& col_range,
      const bool is_bitwise_eq,
      const std::pair<const Analyzer::ColumnVar*, const Analyzer::Expr*>& cols,
      const HashEntryInfo hash_entry_info,
      const int32_t hash_join_invalid_val,
      const Executor* executor) {
    auto timer = DEBUG_TIMER(__func__);
    const auto inner_col = cols.first;
    CHECK(inner_col);
    const auto& ti = inner_col->get_type_info();

    auto cpu_hash_table_buff = std::make_shared<std::vector<int32_t>>(
        2 * hash_entry_info.getNormalizedHashEntryCount() + join_column.num_elems);
    const StringDictionaryProxy* sd_inner_proxy{nullptr};
    const StringDictionaryProxy* sd_outer_proxy{nullptr};
    if (ti.is_string()) {
      CHECK_EQ(kENCODING_DICT, ti.get_compression());
      sd_inner_proxy =
          executor->getStringDictionaryProxy(inner_col->get_comp_param(), true);
      CHECK(sd_inner_proxy);
      const auto outer_col = dynamic_cast<const Analyzer::ColumnVar*>(cols.second);
      CHECK(outer_col);
      sd_outer_proxy =
          executor->getStringDictionaryProxy(outer_col->get_comp_param(), true);
      CHECK(sd_outer_proxy);
    }
    int thread_count = cpu_threads();
    std::vector<std::future<void>> init_threads;
    for (int thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
      init_threads.emplace_back(std::async(std::launch::async,
                                           init_hash_join_buff,
                                           cpu_hash_table_buff->data(),
                                           hash_entry_info.getNormalizedHashEntryCount(),
                                           hash_join_invalid_val,
                                           thread_idx,
                                           thread_count));
    }
    for (auto& child : init_threads) {
      child.wait();
    }
    for (auto& child : init_threads) {
      child.get();
    }

    if (ti.get_type() == kDATE) {
      fill_one_to_many_hash_table_bucketized(cpu_hash_table_buff->data(),
                                             hash_entry_info,
                                             hash_join_invalid_val,
                                             join_column,
                                             {static_cast<size_t>(ti.get_size()),
                                              col_range.getIntMin(),
                                              col_range.getIntMax(),
                                              inline_fixed_encoding_null_val(ti),
                                              is_bitwise_eq,
                                              col_range.getIntMax() + 1,
                                              get_join_column_type_kind(ti)},
                                             sd_inner_proxy,
                                             sd_outer_proxy,
                                             thread_count);
    } else {
      fill_one_to_many_hash_table(cpu_hash_table_buff->data(),
                                  hash_entry_info,
                                  hash_join_invalid_val,
                                  join_column,
                                  {static_cast<size_t>(ti.get_size()),
                                   col_range.getIntMin(),
                                   col_range.getIntMax(),
                                   inline_fixed_encoding_null_val(ti),
                                   is_bitwise_eq,
                                   col_range.getIntMax() + 1,
                                   get_join_column_type_kind(ti)},
                                  sd_inner_proxy,
                                  sd_outer_proxy,
                                  thread_count);
    }

    CHECK(hash_table_);
    hash_table_->setHashTableBuffer(cpu_hash_table_buff);
  }

  std::unique_ptr<PerfectHashTable> getHashTable() { return std::move(hash_table_); }

  static size_t get_entries_per_shard(const size_t total_entry_count,
                                      const size_t shard_count) {
    CHECK_NE(size_t(0), shard_count);
    return (total_entry_count + shard_count - 1) / shard_count;
  }

 private:
  std::unique_ptr<PerfectHashTable> hash_table_;
};
