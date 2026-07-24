#pragma once

#include <muduo/base/Mutex.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dvk {

// ============================================================
// KVStore — 分片锁内存 KV 存储
//
// 设计:
//   - 16 个 Shard, 每个有独立的 mutex
//   - key → shardIndex(key) = hash(key) % 16
//   - 不同 shard 可并发读写, 仅同 shard 的 key 才竞争锁
//   - 相比全局锁, 16 路分片让冲突概率降到 1/16
//
// 每个 Shard 内部是 std::unordered_map<string, string>,
// O(1) 查找/插入/删除。
// ============================================================
class KVStore {
public:
  // 16片
  static constexpr size_t NUM_SHARDS = 16;

  // SET: 写入键值对 (覆盖已有值)
  void set(const std::string &key, const std::string &value) {
    Shard &s = getShard(key);
    muduo::MutexLockGuard lock(s.mutex);
    s.data[key] = value;
  }

  // GET: 读取键值, 不存在则返回 nullopt
  std::optional<std::string> get(const std::string &key) const {
    Shard &s = getShard(key);
    // 自动锁
    muduo::MutexLockGuard lock(s.mutex);
    auto it = s.data.find(key);
    if (it != s.data.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // DEL: 删除键, 返回 true 表示键存在并被删除
  bool del(const std::string &key) {
    Shard &s = getShard(key);
    muduo::MutexLockGuard lock(s.mutex);
    return s.data.erase(key) > 0;
  }

  // 返回存储中的总键数 (遍历所有分片求和)
  size_t size() const {
    size_t total = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
      muduo::MutexLockGuard lock(shards_[i].mutex);
      total += shards_[i].data.size();
    }
    return total;
  }

private:
  struct Shard {
    std::unordered_map<std::string, std::string> data; // store kv
    mutable muduo::MutexLock mutex;
  };

  Shard &getShard(const std::string &key) const {
    size_t idx = std::hash<std::string>{}(key) % NUM_SHARDS;
    return shards_[idx];
  }

  mutable Shard shards_[NUM_SHARDS];
};

} // namespace dvk
