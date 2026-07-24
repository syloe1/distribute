#pragma once

#include "common.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpClient.h>

#include <memory>
#include <string>
#include <vector>

namespace dvk {

// ============================================================
// Replicator — 异步主从复制 (star 模式)
//
// 主节点收到写请求 (SET/DEL) 后:
//   1. 先写入本地 KVStore
//   2. 调用 replicateWrite / replicateDelete 异步转发到所有从节点
//   3. 对客户端返回 OK (不等从节点确认)
//
// 容错:
//   - 从节点断开 → 静默丢弃本应发给它的复制消息
//   - TcpClient 启用 auto-retry, 从节点恢复后自动重连
//   - 这种设计是 AP (高可用 + 分区容忍), 牺牲 C (强一致性)
//
// 防循环:
//   - 从节点不配置 Replicator, 收到复制命令只写本地
//   - SET 天然幂等, 重复写入不会出错
// ============================================================
class Replicator {
public:
  // 每个从节点单独创建一个Muduo TcpClient， 配置自动重连， 连接状态回调
  Replicator(muduo::net::EventLoop *loop, const std::vector<Node> &replicas)
      : loop_(loop) {
    // 遍历集群里每一台从机， 一台从机对应一条独立TCP连接
    for (const auto &node : replicas) {
      muduo::net::InetAddress addr(node.host, node.port);
      // TcpClient智能指针
      auto client = std::make_unique<muduo::net::TcpClient>(
          loop, addr, "ReplicaTo-" + node.id);
      client->enableRetry(); // 断开后自动重连
      client->setConnectionCallback(
          // lambda捕获node.id副本
          //[this, &node] 引用捕获， 循环迭代node会变
          [this, nodeId = node.id](const muduo::net::TcpConnectionPtr &conn) {
            if (conn->connected()) { // 成功连接撒谎给你从节点
              LOG_INFO << "Replicator: connected to replica " << nodeId;
            } else {
              LOG_WARN << "Replicator: disconnected from replica " << nodeId;
            }
          });
      // unique so move to tranfer owner
      replicas_.emplace_back(node, std::move(client));
    }
  }

  /*
 // 启动所有到从节点的连接
  void start() {
    for (auto &rc : replicas_) {
      rc.client->connect();
    }

*/
  // 启动所有到从节点的连接
  void start() {
    for (auto &rc : replicas_) {
      // 每轮抽一个ReplicaConn对象
      rc.client->connect();
    }
  }

  // 停止所有连接
  void stop() {
    for (auto &rc : replicas_) {
      rc.client->disconnect();
    }
  }

  // 异步复制 SET (fire-and-forget)
  void replicateWrite(const std::string &key, const std::string &value) {
    std::string cmd = Protocol::serializeSET(key, value);
    for (auto &rc : replicas_) {
      auto conn = rc.client->connection();
      if (conn && conn->connected()) {
        conn->send(cmd);
      }
    }
  }

  // 异步复制 DEL (fire-and-forget)
  void replicateDelete(const std::string &key) {
    std::string cmd = Protocol::serializeDEL(key);
    for (auto &rc : replicas_) {
      auto conn = rc.client->connection();
      if (conn && conn->connected()) {
        conn->send(cmd);
      }
    }
  }

private:
  // 从节点元信息 + 对应TCP客户端打包存在一起
  struct ReplicaConn {
    Node node;
    std::unique_ptr<muduo::net::TcpClient> client;

    ReplicaConn() = default;
    ReplicaConn(const Node &n, std::unique_ptr<muduo::net::TcpClient> &&c)
        : node(n), client(std::move(c)) {}
    ReplicaConn(ReplicaConn &&) = default;
  };

  muduo::net::EventLoop *loop_;       // 事件循环
  std::vector<ReplicaConn> replicas_; // 所有从节点连接信息
};

} // namespace dvk
