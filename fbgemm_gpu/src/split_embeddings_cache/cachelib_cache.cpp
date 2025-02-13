/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "fbgemm_gpu/split_embeddings_cache/cachelib_cache.h"
#include "fbgemm_gpu/split_embeddings_cache/kv_db_cpp_utils.h"
#include "fbgemm_gpu/utils/dispatch_macros.h"

namespace l2_cache {

using Cache = facebook::cachelib::LruAllocator;
CacheLibCache::CacheLibCache(size_t cacheSizeBytes, int64_t num_shards)
    : cache_config_(CacheConfig{.cacheSizeBytes = cacheSizeBytes}),
      cache_(initializeCacheLib(cache_config_)),
      admin_(createCacheAdmin(*cache_)) {
  for (int i = 0; i < num_shards; i++) {
    pool_ids_.push_back(cache_->addPool(
        fmt::format("shard_{}", i),
        cache_->getCacheMemoryStats().ramCacheSize / num_shards));
  }
}

std::unique_ptr<Cache> CacheLibCache::initializeCacheLib(
    const CacheConfig& config) {
  auto eviction_cb =
      [this](const facebook::cachelib::LruAllocator::RemoveCbData& data) {
        FBGEMM_DISPATCH_FLOAT_HALF_AND_BYTE(
            evicted_weights_ptr_->scalar_type(), "l2_eviction_handling", [&] {
              if (data.context ==
                  facebook::cachelib::RemoveContext::kEviction) {
                auto indices_data_ptr =
                    evicted_indices_ptr_->data_ptr<int64_t>();
                auto weights_data_ptr =
                    evicted_weights_ptr_->data_ptr<scalar_t>();
                auto row_id = eviction_row_id++;
                auto weight_dim = evicted_weights_ptr_->size(1);
                const auto key_ptr =
                    reinterpret_cast<const int64_t*>(data.item.getKey().data());
                indices_data_ptr[row_id] = *key_ptr;

                std::copy(
                    reinterpret_cast<const scalar_t*>(data.item.getMemory()),
                    reinterpret_cast<const scalar_t*>(data.item.getMemory()) +
                        weight_dim,
                    &weights_data_ptr[row_id * weight_dim]); // dst_start
              }
            });
      };
  Cache::Config cacheLibConfig;
  cacheLibConfig.setCacheSize(static_cast<uint64_t>(config.cacheSizeBytes))
      .setRemoveCallback(eviction_cb)
      .setCacheName("TBEL2Cache")
      .setAccessConfig({25 /* bucket power */, 10 /* lock power */})
      .setFullCoredump(false)
      .validate();
  return std::make_unique<Cache>(cacheLibConfig);
}

std::unique_ptr<facebook::cachelib::CacheAdmin> CacheLibCache::createCacheAdmin(
    Cache& cache) {
  facebook::cachelib::CacheAdmin::Config adminConfig;
  adminConfig.oncall = "mvai";
  return std::make_unique<facebook::cachelib::CacheAdmin>(
      cache, std::move(adminConfig));
}

std::optional<void*> CacheLibCache::get(int64_t key) {
  auto key_str =
      folly::StringPiece(reinterpret_cast<const char*>(&key), sizeof(int64_t));
  auto item = cache_->find(key_str);
  if (!item) {
    return std::nullopt;
  }
  return const_cast<void*>(item->getMemory());
}

size_t CacheLibCache::get_shard_id(int64_t key) {
  return kv_db_utils::hash_shard(key, pool_ids_.size());
}

facebook::cachelib::PoolId CacheLibCache::get_pool_id(int64_t key) {
  return pool_ids_[get_shard_id(key)];
}

bool CacheLibCache::put(int64_t key, const at::Tensor& data) {
  auto key_str =
      folly::StringPiece(reinterpret_cast<const char*>(&key), sizeof(int64_t));
  auto item = cache_->allocate(get_pool_id(key), key_str, data.nbytes());
  if (!item) {
    XLOG(ERR) << fmt::format("Failed to allocate item {} in cache, skip", key);
    return false;
  }
  std::memcpy(item->getMemory(), data.data_ptr(), data.nbytes());
  cache_->insertOrReplace(std::move(item));
  return true;
}

void CacheLibCache::init_tensor_for_l2_eviction(
    const at::Tensor& indices,
    const at::Tensor& weights,
    const at::Tensor& count) {
  auto num_lookups = count.item<long>();
  evicted_indices_ptr_ = std::make_shared<at::Tensor>(
      at::ones(
          num_lookups,
          at::TensorOptions().device(indices.device()).dtype(indices.dtype())) *
      -1);
  evicted_weights_ptr_ = std::make_shared<at::Tensor>(at::empty(
      {num_lookups, weights.size(1)},
      at::TensorOptions().device(weights.device()).dtype(weights.dtype())));
}

void CacheLibCache::reset_eviction_states() {
  eviction_row_id = 0;
}

folly::Optional<std::pair<at::Tensor, at::Tensor>>
CacheLibCache::get_evicted_indices_and_weights() {
  if (evicted_indices_ptr_) {
    assert(evicted_weights_ptr_ != nullptr);
    return std::make_pair(*evicted_indices_ptr_, *evicted_weights_ptr_);
  } else {
    return folly::none;
  }
}

std::vector<int64_t> CacheLibCache::get_cache_usage() {
  std::vector<int64_t> cache_mem_stats(2, 0); // freeBytes, capacity
  cache_mem_stats[1] = cache_config_.cacheSizeBytes;
  for (auto& pool_id : pool_ids_) {
    auto pool_stats = cache_->getPoolStats(pool_id);
    cache_mem_stats[0] += pool_stats.freeMemoryBytes();
  }
  return cache_mem_stats;
}

} // namespace l2_cache
