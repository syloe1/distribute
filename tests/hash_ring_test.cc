// ============================================================
// hash_ring_test.cc — HashRing 单元测试
//
// 测试范围:
//   1. 空环行为
//   2. 单节点 / 多节点路由
//   3. 确定性 (同一 key 总是到同一节点)
//   4. 分布均匀性 (10000 key, 标准差 < 15%)
//   5. 节点增减迁移率
// ============================================================

#include "hash_ring.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dvk;

// ---- 简易测试框架 (与 kv_store_test 相同) ----
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)                                                            \
  static void test_##name();                                                  \
  struct Registrar_##name {                                                   \
    Registrar_##name() { registerTest(#name, test_##name); }                  \
  } registrar_##name;                                                         \
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

// ---- 辅助函数 ----
static std::vector<Node> makeNodes(int count, int basePort = 7001) {
  std::vector<Node> nodes;
  for (int i = 0; i < count; ++i) {
    Node n;
    n.id = "node-" + std::to_string(i + 1);
    n.host = "127.0.0.1";
    n.port = static_cast<uint16_t>(basePort + i);
    nodes.push_back(n);
  }
  return nodes;
}

// ============================================================
// 1. 空环
// ============================================================

TEST(empty_ring_returns_null) {
  HashRing ring;
  EXPECT(ring.empty());
  EXPECT(ring.vnodeCount() == 0);
  EXPECT(ring.getNode("any_key") == nullptr);
}

// ============================================================
// 2. 单节点
// ============================================================

TEST(single_node_all_keys_route_to_it) {
  HashRing ring;
  auto nodes = makeNodes(1);
  ring.build(nodes);

  EXPECT(!ring.empty());
  EXPECT(ring.vnodeCount() == HashRing::VIRTUAL_NODES_PER_PHYSICAL);

  // 任何 key 都应路由到这个节点
  for (int i = 0; i < 100; ++i) {
    const Node *node = ring.getNode("key_" + std::to_string(i));
    EXPECT(node != nullptr);
    EXPECT(node->id == "node-1");
  }
}

// ============================================================
// 3. 确定性路由
// ============================================================

TEST(deterministic_routing) {
  HashRing ring;
  ring.build(makeNodes(3));

  // 同一 key 多次查询返回同一节点
  std::string key = "my_consistent_key";
  std::string firstNode;
  for (int i = 0; i < 100; ++i) {
    const Node *node = ring.getNode(key);
    EXPECT(node != nullptr);
    if (i == 0) {
      firstNode = node->id;
    } else {
      EXPECT(node->id == firstNode);
    }
  }
}

// ============================================================
// 4. 所有 key 都能路由 (不返回 nullptr)
// ============================================================

TEST(all_keys_routable) {
  HashRing ring;
  ring.build(makeNodes(3));

  for (int i = 0; i < 1000; ++i) {
    const Node *node = ring.getNode("k" + std::to_string(i));
    EXPECT(node != nullptr);
    EXPECT(!node->id.empty());
  }
}

// ============================================================
// 5. 分布均匀性
// ============================================================

TEST(distribution_uniformity) {
  HashRing ring;
  auto nodes = makeNodes(3);
  ring.build(nodes);

  const int numKeys = 10000;
  std::unordered_map<std::string, int> counts;

  for (int i = 0; i < numKeys; ++i) {
    const Node *node = ring.getNode("benchmark_key_" + std::to_string(i));
    counts[node->id]++;
  }

  // 每个节点都应分配到 key
  for (const auto &n : nodes) {
    EXPECT(counts[n.id] > 0);
  }

  // 计算标准差
  double mean = static_cast<double>(numKeys) / nodes.size();
  double variance = 0.0;
  for (const auto &n : nodes) {
    double diff = counts[n.id] - mean;
    variance += diff * diff;
  }
  variance /= nodes.size();
  double stddev = std::sqrt(variance);
  double cv = stddev / mean; // 变异系数

  std::cout << "\n    Distribution: ";
  for (const auto &n : nodes) {
    double pct = 100.0 * counts[n.id] / numKeys;
    std::cout << n.id << "=" << counts[n.id] << " (" << pct << "%) ";
  }
  std::cout << " CV=" << (cv * 100) << "%";

  // 变异系数应 < 15% (3节点 * 150 vnode 通常 < 10%)
  EXPECT(cv < 0.15);
}

// ============================================================
// 6. 增加节点 — 迁移率
// ============================================================

TEST(add_node_migration) {
  // 从 2 节点开始
  HashRing ring2;
  auto nodes2 = makeNodes(2);
  ring2.build(nodes2);

  const int numKeys = 5000;
  std::unordered_map<std::string, std::string> keyToNode;

  // 记录 2 节点时的路由
  for (int i = 0; i < numKeys; ++i) {
    std::string key = "migrate_" + std::to_string(i);
    const Node *n = ring2.getNode(key);
    keyToNode[key] = n->id;
  }

  // 扩展到 3 节点
  HashRing ring3;
  auto nodes3 = makeNodes(3);
  ring3.build(nodes3);

  // 统计变化
  int migrated = 0;
  for (int i = 0; i < numKeys; ++i) {
    std::string key = "migrate_" + std::to_string(i);
    const Node *n = ring3.getNode(key);
    if (n->id != keyToNode[key]) {
      migrated++;
    }
  }

  double migrationRate = static_cast<double>(migrated) / numKeys;
  std::cout << "\n    Migration rate (2→3 nodes): " << (migrationRate * 100)
            << "%";

  // 理论上约 1/3 的 key 需要迁移。实际因虚拟节点分布略有偏差，
  // 保守预计应在 20% ~ 50% 之间
  EXPECT(migrationRate > 0.15);
  EXPECT(migrationRate < 0.55);
}

// ============================================================
// 7. 移除节点 — 剩余 key 仍可路由
// ============================================================

TEST(remove_node_routing) {
  // 3 节点环
  HashRing ring3;
  auto nodes3 = makeNodes(3);
  ring3.build(nodes3);

  const int numKeys = 1000;
  std::unordered_map<std::string, std::string> routes3;
  for (int i = 0; i < numKeys; ++i) {
    std::string key = "remove_" + std::to_string(i);
    routes3[key] = ring3.getNode(key)->id;
  }

  // 缩减到 2 节点（移除 node-3）
  HashRing ring2;
  auto nodes2 = makeNodes(2);
  ring2.build(nodes2);

  int migrated = 0;
  std::unordered_map<std::string, int> newCounts;
  for (int i = 0; i < numKeys; ++i) {
    std::string key = "remove_" + std::to_string(i);
    const Node *n = ring2.getNode(key);
    EXPECT(n != nullptr); // 所有 key 仍可路由
    newCounts[n->id]++;
    if (n->id != routes3[key]) {
      migrated++;
    }
  }

  double migRate = static_cast<double>(migrated) / numKeys;
  std::cout << "\n    Migration rate (3→2 nodes): " << (migRate * 100) << "%";

  // node-3 上的 key 应迁移到剩余节点
  EXPECT(migRate > 0.05);

  // 两节点都有 key
  EXPECT(newCounts["node-1"] > 0);
  EXPECT(newCounts["node-2"] > 0);
}

// ============================================================
// 8. rebuild — 正确的虚拟节点数
// ============================================================

TEST(vnode_count_per_node) {
  HashRing ring;
  ring.build(makeNodes(3));
  EXPECT(ring.vnodeCount() ==
         static_cast<size_t>(3 * HashRing::VIRTUAL_NODES_PER_PHYSICAL));

  ring.build(makeNodes(5));
  EXPECT(ring.vnodeCount() ==
         static_cast<size_t>(5 * HashRing::VIRTUAL_NODES_PER_PHYSICAL));

  // 空 rebuild
  ring.build({});
  EXPECT(ring.vnodeCount() == 0);
  EXPECT(ring.empty());
}

// ============================================================
// 9. rebuild 覆盖旧环
// ============================================================

TEST(rebuild_replaces_old_ring) {
  HashRing ring;
  ring.build(makeNodes(5));
  EXPECT(ring.vnodeCount() == 5 * 150);

  ring.build(makeNodes(2));
  EXPECT(ring.vnodeCount() == 2 * 150);

  // 新 key 应只路由到新节点
  const Node *n = ring.getNode("test");
  EXPECT(n != nullptr);
  EXPECT(n->id == "node-1" || n->id == "node-2");
}

// ============================================================
// main
// ============================================================

int main() {
  std::cout << "=== HashRing Unit Tests ===\n";

  for (const auto &entry : allTests()) {
    std::cout << "  " << entry.name << " ...";
    int before = g_passed + g_failed;
    entry.func();
    int after = g_passed + g_failed;
    if (after == before) {
      std::cout << " PASS\n";
    }
  }

  std::cout << "\nResults: " << g_passed << " passed, " << g_failed
            << " failed\n";
  return g_failed > 0 ? 1 : 0;
}
