#pragma once

#include "common.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <openssl/md5.h> //OpenSSL MD5哈希函数
#include <string>
#include <vector>

namespace dvk {

// ============================================================
// HashRing — 一致性哈希环
//
// 算法:
//   1. 每个物理节点生成 VIRTUAL_NODES_PER_PHYSICAL 个虚拟节点
//   2. 虚拟节点名 = "node_id:vn_N", 用 MD5 哈希映射到 uint64_t 位置
//   3. 键查找: hash(key) → ring_.upper_bound(h) → 返回最近的物理节点
//   4. 如果越过环末尾, 回绕到 ring_.begin()
//
// 复杂度:
//   - 构建: O(N * V) (N=节点数, V=虚拟节点数)
//   - 查找: O(log(N * V))
//
// 数据分布:
//   3 节点 × 150 vnode, 标准差约 5%~10%
// ============================================================
class HashRing {
public:
  // 编译器常量
  // 每个真实物理节点生成150个虚拟节点
  static constexpr int VIRTUAL_NODES_PER_PHYSICAL = 150;

  // 用节点列表构建哈希环
  void build(const std::vector<Node> &nodes) {
    ring_.clear();
    for (const auto &node : nodes) {
      for (int i = 0; i < VIRTUAL_NODES_PER_PHYSICAL; ++i) {
        std::string vname = node.id + ":vn_" + std::to_string(i);
        uint64_t pos = md5Hash64(vname);
        ring_[pos] = node;
      }
    }
  }

  // 根据 key 查找所属节点。环为空时返回 nullptr。
  const Node *getNode(const std::string &key) const {
    if (ring_.empty())
      return nullptr;

    uint64_t h = md5Hash64(key);
    auto it = ring_.upper_bound(h);
    if (it == ring_.end()) {
      it = ring_.begin(); // 回绕
    }
    return &it->second;
  }

  bool empty() const { return ring_.empty(); }
  size_t vnodeCount() const { return ring_.size(); }

private:
  // MD5 哈希 → 取其高 8 字节作为 uint64_t (big-endian)
  static uint64_t md5Hash64(const std::string &input) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    // GCC 警告屏蔽代码块
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    MD5(reinterpret_cast<const unsigned char *>(input.data()), input.size(),
        digest);
#pragma GCC diagnostic pop

    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
      result = (result << 8) | digest[i];
    }
    return result;
  }

  // 有序映射: hash_position → Node
  // 利用 std::map 的 O(log N) upper_bound 查找
  std::map<uint64_t, Node> ring_;
  /*
    struct Node {
    // name + ip + port
    std::string id;    // e.g. "node-1"
    std::string host;  // e.g. "127.0.0.1"
                    // uint16_t 2个字节
    uint16_t port = 0; // e.g. 7001

    std::string address() const { return host + ":" + std::to_string(port); }
    };

*/
};

} // namespace dvk
