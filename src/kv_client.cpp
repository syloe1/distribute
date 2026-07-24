// ============================================================
// kv_client.cpp — 分布式 KV 客户端
//
// 用法:
//   ./kv_client 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003
//
// 功能:
//   - 一致性哈希路由: key → hash → 确定目标节点
//   - 每个节点一个 TcpClient 长连接
//   - 交互式 REPL 界面 (主线程)
//   - I/O 线程处理网络读写
//   - 主线程与 I/O 线程通过 std::promise/future 通信
// ============================================================

#include "common.h"
#include "hash_ring.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpClient.h>

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace muduo;
using namespace muduo::net;

namespace dvk {

// ============================================================
// KVClient — 一致性哈希路由客户端
// ============================================================
class KVClient {
public:
  KVClient(EventLoop *loop, const std::vector<Node> &nodes) : loop_(loop) {
    ring_.build(nodes); // 1. 构建一致性哈希环，用于key分片路由

    // 遍历所有集群节点，每个节点独立创建TcpClient
    for (const auto &node : nodes) {
      InetAddress addr(node.host, node.port);
      // 创建独占智能指针TcpClient，绑定事件循环、目标地址、连接名称
      auto client =
          std::make_unique<TcpClient>(loop, addr, "KVClient-" + node.id);
      client->enableRetry(); // 开启断线自动后台重连

      // 绑定连接状态回调（连上/断开更新conn指针）
      client->setConnectionCallback(
          [this, nid = node.id](const TcpConnectionPtr &conn) {
            onNodeConnection(nid, conn);
          });
      // 绑定消息回调：收到服务端返回数据触发
      client->setMessageCallback(
          [this, nid = node.id](const TcpConnectionPtr &conn, Buffer *buf,
                                Timestamp t) {
            onNodeMessage(nid, conn, buf, t);
          });

      // 包装成共享指针存入map
      auto nc = std::make_shared<NodeConnection>();
      nc->node = node;
      nc->client = std::move(client); // move转移unique_ptr所有权
      connections_[node.id] = nc;
    }
  }

  void start() { // 启动连接start()
    for (auto &[id, nc] : connections_) {
      nc->client->connect();
    }
    // 等待连接建立
    CurrentThread::sleepUsec(500 * 1000);
  }

  void stop() { // 停止连接start()
    for (auto &[id, nc] : connections_) {
      nc->client->disconnect();
    }
  }

  // ---- 同步 API (内部用 promise/future 桥接) ----

  std::string syncSET(const std::string &key, const std::string &value) {
    std::string cmd = "SET " + key + " " + value + "\r\n";
    return sendToNodeForKey(key, cmd);
  }

  std::string syncGET(const std::string &key) {
    std::string cmd = "GET " + key + "\r\n";
    return sendToNodeForKey(key, cmd);
  }

  std::string syncDEL(const std::string &key) {
    std::string cmd = "DEL " + key + "\r\n";
    return sendToNodeForKey(key, cmd);
  }

  const HashRing &ring() const { return ring_; }

private:
  struct NodeConnection {
    Node node;                                          // 节点信息 id/host/port
    std::unique_ptr<TcpClient> client;                  // 连接该节点的TCP客户端
    TcpConnectionPtr conn;                              // 当前活跃TCP连接
    std::mutex mtx;                                     // 保护promise多线程竞争
    std::unique_ptr<std::promise<std::string>> promise; // 同步等待器
  };

  // 根据 key 定位节点并发命令
  // 同一个 key 永远路由到同一台机器，分布式分片核心。
  std::string sendToNodeForKey(const std::string &key, const std::string &cmd) {
    const Node *node = ring_.getNode(key);
    if (!node) {
      return "-ERR no available node\r\n";
    }
    return sendToNode(*node, cmd);
  }
  // 阻塞等待IO线程
  std::string sendToNode(const Node &node, const std::string &cmd) {
    auto it = connections_.find(node.id);
    if (it == connections_.end()) {
      return "-ERR node not found\r\n";
    }

    auto &nc = it->second;

    // 创建 promise 用于等待响应
    auto prom = std::make_unique<std::promise<std::string>>();
    auto future = prom->get_future();
    {
      std::lock_guard<std::mutex> lock(nc->mtx);
      nc->promise = std::move(prom);
    }

    // 在 I/O 线程发送命令
    loop_->runInLoop([nc, cmd]() {
      if (nc->conn && nc->conn->connected()) {
        nc->conn->send(cmd);
      }
    });

    // 等待响应，超时 5 秒
    auto status = future.wait_for(std::chrono::seconds(5));
    if (status == std::future_status::timeout) {
      std::lock_guard<std::mutex> lock(nc->mtx);
      nc->promise.reset();
      return "-ERR timeout\r\n";
    }
    return future.get();
  }
  // 连接回调函数
  void onNodeConnection(const std::string &nodeId,
                        const TcpConnectionPtr &conn) {
    auto it = connections_.find(nodeId);
    if (it == connections_.end())
      return;

    if (conn->connected()) {
      it->second->conn = conn;
      LOG_INFO << "Connected to " << nodeId;
    } else {
      it->second->conn.reset();
      LOG_INFO << "Disconnected from " << nodeId;
    }
  }

  void onNodeMessage(const std::string &nodeId, const TcpConnectionPtr &,
                     Buffer *buf, Timestamp) {
    auto it = connections_.find(nodeId);
    if (it == connections_.end())
      return;

    std::string response = buf->retrieveAllAsString();
    auto &nc = it->second;

    std::lock_guard<std::mutex> lock(nc->mtx);
    if (nc->promise) {
      nc->promise->set_value(response);
      nc->promise.reset();
    }
  }

  EventLoop *loop_;
  HashRing ring_;
  std::unordered_map<std::string, std::shared_ptr<NodeConnection>> connections_;
};

} // namespace dvk

// ============================================================
// 命令行解析
// ============================================================
std::vector<dvk::Node> parseNodes(int argc, char *argv[]) {
  std::vector<dvk::Node> nodes;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    auto colonPos = arg.find(':');
    if (colonPos != std::string::npos) {
      dvk::Node n;
      n.host = arg.substr(0, colonPos);
      n.port = static_cast<uint16_t>(std::stoi(arg.substr(colonPos + 1)));
      n.id = "node-" + std::to_string(i); // node-1, node-2, ...
      nodes.push_back(n);
    }
  }
  return nodes;
}

// ============================================================
// REPL
// ============================================================
void printHelp() {
  std::cout << "\n"
            << "===== Distributed KV Client =====\n"
            << "Commands:\n"
            << "  SET <key> <value>   — 写入键值\n"
            << "  GET <key>           — 读取键值\n"
            << "  DEL <key>           — 删除键\n"
            << "  NODES               — 查看集群节点\n"
            << "  QUIT                — 退出\n"
            << "==================================\n\n";
}
// 清理用户输入多余前后空格、换行，避免命令解析出错。
std::string trim(const std::string &s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <host:port> [host:port ...]\n"
              << "Example: " << argv[0]
              << " 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003\n";
    return 1;
  }

  auto nodes = parseNodes(argc, argv);
  if (nodes.empty()) {
    std::cerr << "No valid node addresses provided.\n";
    return 1;
  }

  Logger::setLogLevel(Logger::WARN);

  // 网络 I/O 跑在独立 EventLoop 线程
  EventLoopThread loopThread;
  EventLoop *loop = loopThread.startLoop();

  dvk::KVClient client(loop, nodes);
  client.start();

  std::cout << "Connected to " << nodes.size() << " node(s), "
            << client.ring().vnodeCount()
            << " virtual nodes on the hash ring.\n";
  printHelp();

  // 主线程 REPL
  std::string line;
  while (true) {
    std::cout << "kv> " << std::flush;
    if (!std::getline(std::cin, line))
      break;

    line = trim(line);
    if (line.empty())
      continue;
    if (line == "QUIT" || line == "quit")
      break;

    if (line == "NODES" || line == "nodes") {
      std::cout << "Nodes in cluster (" << nodes.size() << "):\n";
      for (const auto &n : nodes) {
        std::cout << "  " << n.id << " @ " << n.address() << "\n";
      }
      std::cout << "Virtual nodes (ring size): " << client.ring().vnodeCount()
                << "\n";
      continue;
    }

    // 解析命令并执行
    std::istringstream iss(line);
    std::string cmd, key, value;
    iss >> cmd;

    std::string resp;
    if (cmd == "SET" || cmd == "set") {
      iss >> key;
      std::getline(iss, value);
      value = trim(value);
      if (key.empty()) {
        std::cout << "(error) Usage: SET <key> <value>\n";
        continue;
      }
      resp = client.syncSET(key, value);
    } else if (cmd == "GET" || cmd == "get") {
      iss >> key;
      if (key.empty()) {
        std::cout << "(error) Usage: GET <key>\n";
        continue;
      }
      resp = client.syncGET(key);
    } else if (cmd == "DEL" || cmd == "del") {
      iss >> key;
      if (key.empty()) {
        std::cout << "(error) Usage: DEL <key>\n";
        continue;
      }
      resp = client.syncDEL(key);
    } else {
      std::cout << "(error) Unknown command: " << cmd << "\n";
      continue;
    }

    // 显示结果
    std::cout << resp; // RESP 格式自带 \r\n
  }

  std::cout << "Goodbye.\n";
  client.stop();
  CurrentThread::sleepUsec(100 * 1000);
  return 0;
}
