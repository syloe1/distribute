// ============================================================
// kv_store_test.cc — KVStore 单元测试
//
// 测试范围:
//   1. 基本 CRUD (SET/GET/DEL)
//   2. 边界条件 (empty store, missing key, overwrite)
//   3. 分片正确性 (size 遍历所有分片)
//   4. 并发读写 (多线程，验证无 data race / 无死锁)
//   5. 同分片竞争 (多线程写同一 shard)
// ============================================================

#include "kv_store.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace dvk;

// ---- 简单的测试框架 ----
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)                              \
  static void test_##name();                    \
  struct Registrar_##name {                     \
    Registrar_##name() {                        \
      registerTest(#name, test_##name);         \
    }                                           \
  } registrar_##name;                           \
  static void test_##name()

using TestFunc = void (*)();
struct TestEntry {
  const char *name;
  TestFunc func;
};
static std::vector<TestEntry> &allTests() {
  static std::vector<TestEntry> tests;
  return tests;
}
void registerTest(const char *name, TestFunc func) {
  allTests().push_back({name, func});
}

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "  FAILED: " << __FILE__ << ":" << __LINE__ << " ["       \
                << #cond << "]\n";                                            \
      g_failed++;                                                             \
      return;                                                                 \
    } else {                                                                  \
      g_passed++;                                                             \
    }                                                                         \
  } while (0)

// ============================================================
// 1. 基本 CRUD
// ============================================================

TEST(set_and_get) {
  KVStore store;
  store.set("hello", "world");
  auto v = store.get("hello");
  EXPECT(v.has_value());
  EXPECT(v.value() == "world");
}

TEST(get_missing_key) {
  KVStore store;
  auto v = store.get("no_such_key");
  EXPECT(!v.has_value());
}

TEST(del_existing) {
  KVStore store;
  store.set("k", "v");
  EXPECT(store.del("k") == true);
  EXPECT(!store.get("k").has_value());
}

TEST(del_missing) {
  KVStore store;
  EXPECT(store.del("nothing") == false);
}

TEST(overwrite) {
  KVStore store;
  store.set("k", "v1");
  store.set("k", "v2");
  auto v = store.get("k");
  EXPECT(v.has_value());
  EXPECT(v.value() == "v2");
}

// ============================================================
// 2. Size 跟踪
// ============================================================

TEST(size_empty) {
  KVStore store;
  EXPECT(store.size() == 0);
}

TEST(size_after_ops) {
  KVStore store;
  EXPECT(store.size() == 0);

  store.set("a", "1");
  EXPECT(store.size() == 1);

  store.set("b", "2");
  store.set("c", "3");
  EXPECT(store.size() == 3);

  // 覆盖不应改变 size
  store.set("a", "one");
  EXPECT(store.size() == 3);

  store.del("b");
  EXPECT(store.size() == 2);

  // 删除不存在的 key，size 不变
  store.del("nonexist");
  EXPECT(store.size() == 2);
}

// ============================================================
// 3. 分片路由一致性 (同一 key 总是到同一分片)
// ============================================================

TEST(same_key_same_shard) {
  // 间接验证：多次设置后 get 能正确读取（说明 key 路由稳定）
  KVStore store;
  for (int i = 0; i < 100; ++i) {
    store.set("stable", std::to_string(i));
  }
  auto v = store.get("stable");
  EXPECT(v.has_value());
  EXPECT(v.value() == "99");
}

// ============================================================
// 4. 并发 — 不同 key 分布到不同分片，无竞争
// ============================================================

TEST(concurrent_different_shards) {
  KVStore store;
  const int numThreads = 8;
  const int opsPerThread = 5000;
  std::atomic<bool> done{false};

  auto writer = [&](int tid) {
    for (int i = 0; i < opsPerThread; ++i) {
      // 每个线程用不同的 key 前缀，大概率分布到不同分片
      std::string key = "thread_" + std::to_string(tid) + "_" + std::to_string(i);
      store.set(key, std::to_string(i));
    }
  };

  auto reader = [&](int tid) {
    // 持续读，直到 done
    int reads = 0;
    while (!done.load(std::memory_order_relaxed) && reads < opsPerThread) {
      store.get("thread_" + std::to_string(tid) + "_0");
      reads++;
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i) {
    threads.emplace_back(writer, i);
    threads.emplace_back(reader, i);
  }

  for (auto &t : threads)
    t.join();
  done.store(true);

  // 验证数据完整性：每个线程的写入都应正确
  for (int tid = 0; tid < numThreads; ++tid) {
    auto v = store.get("thread_" + std::to_string(tid) + "_0");
    EXPECT(v.has_value()); // 至少第一条应存在
  }
}

// ============================================================
// 5. 并发 — 同分片竞争（所有线程写同一批 key）
// ============================================================

TEST(concurrent_same_keys) {
  KVStore store;
  const int numThreads = 8;
  const int opsPerThread = 2000;

  // 所有线程对同一个 key 做读写，测试互斥正确性
  auto worker = [&](int tid) {
    for (int i = 0; i < opsPerThread; ++i) {
      store.set("counter", std::to_string(tid * opsPerThread + i));
      // 读一下确保不崩
      (void)store.get("counter");
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads)
    t.join();

  // "counter" 应存在且有值
  auto v = store.get("counter");
  EXPECT(v.has_value());
}

// ============================================================
// 6. 混合并发 — SET + DEL 交替
// ============================================================

TEST(concurrent_set_and_del) {
  KVStore store;
  const int numKeys = 100;
  const int numThreads = 4;
  const int rounds = 100;

  // 先初始化
  for (int i = 0; i < numKeys; ++i) {
    store.set("key_" + std::to_string(i), "initial");
  }

  std::atomic<int> errors{0};

  auto worker = [&](int tid) {
    for (int r = 0; r < rounds; ++r) {
      for (int i = 0; i < numKeys; ++i) {
        std::string key = "key_" + std::to_string((i + tid * 7) % numKeys);
        if ((i + tid) % 3 == 0) {
          store.del(key);
        } else {
          store.set(key, "t" + std::to_string(tid));
        }
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i)
    threads.emplace_back(worker, i);
  for (auto &t : threads)
    t.join();

  // 不应崩溃，size 在合理范围内
  size_t sz = store.size();
  EXPECT(sz <= static_cast<size_t>(numKeys));
}

// ============================================================
// main — 运行所有测试
// ============================================================

int main() {
  std::cout << "=== KVStore Unit Tests ===\n";

  for (const auto &entry : allTests()) {
    std::cout << "  " << entry.name << " ... ";
    int before = g_passed + g_failed;
    entry.func();
    int after = g_passed + g_failed;
    if (after == before) {
      std::cout << "PASS\n";
    }
  }

  std::cout << "\nResults: " << g_passed << " passed, " << g_failed
            << " failed\n";
  return g_failed > 0 ? 1 : 0;
}
