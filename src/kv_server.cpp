// ============================================================
// kv_server.cpp — 分布式 KV 存储服务节点
//
// 用法:
//   ./kv_server --id node-1 --port 7001 [--threads 4] [--replicas
//   node-2:7002,node-3:7003]
//
// 功能:
//   - 接收 RESP 文本协议命令 (SET/GET/DEL/PING)
//   - 分片锁内存存储
//   - 可选主从异步复制 (需指定 --replicas)
// ============================================================

#include "common.h"
#include "kv_store.h"
#include "replicator.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpServer.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace muduo;
using namespace muduo::net;

namespace dvk {

// ============================================================
// CommandHandler — 命令分发器
// ============================================================
class CommandHandler {
public:
  CommandHandler(KVStore *store, Replicator *replicator = nullptr)
      : store_(store), replicator_(replicator) {}
  /*
    struct ParsedCommand {
        Command type = Command::UNKNOWN;
        std::string key;
        std::string value;
    };
    */
  std::string handle(const ParsedCommand &cmd) {
    switch (cmd.type) {
    case Command::PING:
      return RESP_PONG;
    case Command::SET:
      return handleSET(cmd);
    case Command::GET:
      return handleGET(cmd);
    case Command::DEL:
      return handleDEL(cmd);
    case Command::UNKNOWN:
      return makeError("unknown command");
    }
    return makeError("internal error");
  }

private:
  std::string handleSET(const ParsedCommand &cmd) {
    // 1. 写本地
    store_->set(cmd.key, cmd.value);

    // 2. 异步复制到从节点
    if (replicator_) {
      replicator_->replicateWrite(cmd.key, cmd.value);
    }
    return RESP_OK;
  }

  std::string handleGET(const ParsedCommand &cmd) {
    auto val = store_->get(cmd.key);
    // 包装成RESP二进制字符串格式返回
    if (val.has_value()) {
      return makeBulkString(val.value());
    }
    return RESP_NIL;
  }

  std::string handleDEL(const ParsedCommand &cmd) {
    bool existed = store_->del(cmd.key);
    // 真正删除了才同步给从节点
    if (existed && replicator_) {
      replicator_->replicateDelete(cmd.key);
    }
    return existed ? RESP_TRUE : RESP_FALSE;
  }

  KVStore *store_;
  Replicator *replicator_;
};

// ============================================================
// KVServer — muduo TcpServer 包装
// ============================================================
class KVServer {
public:
  KVServer(EventLoop *loop, const InetAddress &listenAddr, int numThreads,
           Replicator *replicator = nullptr)
      : server_(loop, listenAddr, "KVServer"), handler_(&store_, replicator) {
    server_.setThreadNum(numThreads);
    server_.setConnectionCallback(std::bind(&KVServer::onConnection, this, _1));
    server_.setMessageCallback(
        std::bind(&KVServer::onMessage, this, _1, _2, _3));
  }

  void start() {
    server_.start();
    LOG_INFO << "KVServer listening on " << server_.ipPort();
  }

  KVStore &store() { return store_; }

private:
  // 连接状态回调
  void onConnection(const TcpConnectionPtr &conn) {
    if (conn->connected()) {
      LOG_INFO << "Connection UP: " << conn->peerAddress().toIpPort();
    } else {
      LOG_INFO << "Connection DOWN: " << conn->peerAddress().toIpPort();
    }
  }
  // 解析回调
  void onMessage(const TcpConnectionPtr &conn, Buffer *buf,
                 Timestamp receiveTime) {
    // 循环解析缓冲区中的所有完整命令
    while (buf->readableBytes() > 0) {
      ParsedCommand cmd;
      if (!Protocol::parseCommand(buf, &cmd)) {
        break; // 半包，等更多数据
      }

      // 分发并发送响应
      std::string response = handler_.handle(cmd);
      conn->send(response);
    }
  }

  TcpServer server_;
  KVStore store_;
  CommandHandler handler_;
};

} // namespace dvk

// ============================================================
// 命令行参数解析 (简易)
// ============================================================
struct ServerArgs {
  std::string id = "node-1"; // 当前节点唯一ID，默认 node-1
  uint16_t port = 7001;      // 监听端口，默认7001，uint16_t 2字节
  int threads = 4;           // muduo IO线程池数量，默认4线程
  std::string replicasStr;   // 从节点字符串，逗号分隔，空=无从节点
};

ServerArgs parseArgs(int argc, char *argv[]) {
  ServerArgs args;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
      args.id = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      args.port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      args.threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--replicas") == 0 && i + 1 < argc) {
      args.replicasStr = argv[++i];
    }
  }
  return args;
}
// 拆分从节点字符串
//  解析 "node-2:7002,node-3:7003" → vector<Node>
std::vector<dvk::Node> parseReplicas(const std::string &s) {
  std::vector<dvk::Node> nodes;
  if (s.empty())
    return nodes;

  std::istringstream iss(s);
  std::string pair;
  while (std::getline(iss, pair, ',')) {
    auto colonPos = pair.find(':');
    if (colonPos != std::string::npos) {
      dvk::Node n;
      n.id = pair.substr(0, colonPos);
      n.host = "127.0.0.1"; // 简化: 默认 localhost
      n.port = static_cast<uint16_t>(std::stoi(pair.substr(colonPos + 1)));
      nodes.push_back(n);
    }
  }
  return nodes;
}

// ============================================================
// main
// ============================================================
int main(int argc, char *argv[]) {
  auto args = parseArgs(argc, argv);
  // 日志配置
  Logger::setLogLevel(Logger::INFO);
  LOG_INFO << "Starting KVServer id=" << args.id << " port=" << args.port
           << " threads=" << args.threads;
  // 创建主事件 + 监听地址
  EventLoop loop;
  InetAddress listenAddr(args.port);

  // 可选: 创建 Replicator
  auto replicaNodes = parseReplicas(args.replicasStr);
  std::unique_ptr<dvk::Replicator> replicator;
  if (!replicaNodes.empty()) {
    replicator = std::make_unique<dvk::Replicator>(&loop, replicaNodes);
    replicator->start();
    LOG_INFO << "Replication enabled to " << replicaNodes.size()
             << " replica(s)";
  }
  // 创建KVServer 服务实例 并 启动监听
  dvk::KVServer server(&loop, listenAddr, args.threads, replicator.get());
  server.start();

  loop.loop();
  return 0;
}
