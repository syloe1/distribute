// ============================================================
// bench.cc — 分布式 KV 压测工具
//
// 用法:
//   1. 先启动服务端:
//        ./build/kv_server --id n1 --port 7001 --threads 4
//
//   2. 运行压测:
//        ./build/bench 127.0.0.1:7001 [--threads 4] [--clients 50] [--duration 10]
//
// 指标:
//   - QPS (每秒操作数)
//   - 延迟分位数 (P50 / P99 / P999)
//   - 错误率
// ============================================================

#include "common.h"

#include <muduo/net/TcpClient.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/InetAddress.h>
#include <muduo/base/Atomic.h>
#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/base/Thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace muduo;
using namespace muduo::net;

// ============================================================
// 统计收集器
// ============================================================
class LatencyStats {
public:
  void record(int64_t us) {
    int idx = g_index.incrementAndGet() % kMaxSamples;
    samples_[idx] = us;
    count_.increment();
  }

  void print() {
    int64_t total = count_.get();
    if (total == 0) {
      std::cout << "  No samples\n";
      return;
    }

    int n = static_cast<int>(std::min(total, static_cast<int64_t>(kMaxSamples)));
    std::vector<int64_t> sorted(samples_, samples_ + n);
    std::sort(sorted.begin(), sorted.end());

    auto pct = [&](double p) -> int64_t {
      int idx = static_cast<int>(n * p);
      if (idx >= n) idx = n - 1;
      return sorted[idx];
    };

    int64_t totalUs = 0;
    for (auto v : sorted) totalUs += v;
    double avgUs = static_cast<double>(totalUs) / n;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Samples : " << n << "\n";
    std::cout << "  Avg     : " << (avgUs / 1000.0) << " ms\n";
    std::cout << "  P50     : " << (pct(0.50) / 1000.0) << " ms\n";
    std::cout << "  P90     : " << (pct(0.90) / 1000.0) << " ms\n";
    std::cout << "  P99     : " << (pct(0.99) / 1000.0) << " ms\n";
    std::cout << "  P999    : " << (pct(0.999) / 1000.0) << " ms\n";
    std::cout << "  Min     : " << (sorted[0] / 1000.0) << " ms\n";
    std::cout << "  Max     : " << (sorted.back() / 1000.0) << " ms\n";
  }

private:
  static constexpr int kMaxSamples = 1000000;
  int64_t samples_[kMaxSamples]{};
  AtomicInt32 g_index;
  AtomicInt64 count_;
};

// ============================================================
// 单个 BenchClient — 一个 TCP 连接
// ============================================================
class BenchClient {
public:
  BenchClient(EventLoop *loop, const InetAddress &serverAddr,
              LatencyStats *stats, int id)
      : loop_(loop), client_(loop, serverAddr, "bench-" + std::to_string(id)),
        stats_(stats), id_(id) {
    client_.setConnectionCallback(
        [this](const TcpConnectionPtr &conn) { onConnection(conn); });
    client_.setMessageCallback(
        [this](const TcpConnectionPtr &, Buffer *buf, Timestamp) {
          onMessage(buf);
        });
  }

  void connect() { client_.connect(); }
  bool connected() const { return conn_ && conn_->connected(); }

  // 异步发送命令（fire and forget 模式用于压测）
  void sendAsync(const std::string &cmd) {
    if (conn_ && conn_->connected()) {
      sendTime_ = Timestamp::now();
      conn_->send(cmd);
    }
  }

  // 同步发送并等待响应（测量延迟）
  void sendAndWait(const std::string &cmd) {
    if (!conn_ || !conn_->connected()) return;

    auto prom = std::make_unique<std::promise<void>>();
    auto fut = prom->get_future();
    {
      std::lock_guard<std::mutex> lock(mtx_);
      promise_ = std::move(prom);
    }

    sendTime_ = Timestamp::now();
    conn_->send(cmd);

    auto status = fut.wait_for(std::chrono::seconds(5));
    if (status == std::future_status::timeout) {
      errors_.increment();
      std::lock_guard<std::mutex> lock(mtx_);
      promise_.reset();
    }
  }

  int64_t errors() { return errors_.get(); }

  void resetErrors() { errors_.getAndSet(0); }

private:
  void onConnection(const TcpConnectionPtr &conn) {
    if (conn->connected()) {
      conn_ = conn;
    } else {
      conn_.reset();
    }
  }

  void onMessage(Buffer *buf) {
    Timestamp now = Timestamp::now();
    int64_t latencyUs =
        (now.microSecondsSinceEpoch() - sendTime_.microSecondsSinceEpoch());
    stats_->record(latencyUs);

    buf->retrieveAll(); // 消费响应

    std::lock_guard<std::mutex> lock(mtx_);
    if (promise_) {
      promise_->set_value();
      promise_.reset();
    }
  }

  EventLoop *loop_;
  TcpClient client_;
  TcpConnectionPtr conn_;
  LatencyStats *stats_;
  Timestamp sendTime_;
  int id_;

  std::mutex mtx_;
  std::unique_ptr<std::promise<void>> promise_;
  AtomicInt64 errors_;
};

// ============================================================
// 参数
// ============================================================
struct BenchArgs {
  std::string host = "127.0.0.1";
  uint16_t port = 7001;
  int threads = 4;
  int clients = 50;
  int durationSec = 10;
};

BenchArgs parseArgs(int argc, char *argv[]) {
  BenchArgs args;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
      args.threads = atoi(argv[++i]);
    else if (strcmp(argv[i], "--clients") == 0 && i + 1 < argc)
      args.clients = atoi(argv[++i]);
    else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
      args.durationSec = atoi(argv[++i]);
    else {
      // 解析 host:port
      std::string arg(argv[i]);
      auto cp = arg.find(':');
      if (cp != std::string::npos) {
        args.host = arg.substr(0, cp);
        args.port = static_cast<uint16_t>(atoi(arg.substr(cp + 1).c_str()));
      }
    }
  }
  return args;
}

// ============================================================
// main
// ============================================================
int main(int argc, char *argv[]) {
  auto args = parseArgs(argc, argv);
  Logger::setLogLevel(Logger::WARN);

  std::cout << "===== Distributed KV Benchmark =====\n";
  std::cout << "Target    : " << args.host << ":" << args.port << "\n";
  std::cout << "Threads   : " << args.threads << "\n";
  std::cout << "Clients   : " << args.clients << "\n";
  std::cout << "Duration  : " << args.durationSec << "s\n";
  std::cout << "====================================\n\n";

  // ---- 创建 IO 线程 ----
  std::vector<std::unique_ptr<EventLoopThread>> ioThreads;
  std::vector<EventLoop *> loops;

  for (int i = 0; i < args.threads; ++i) {
    auto t = std::make_unique<EventLoopThread>();
    loops.push_back(t->startLoop());
    ioThreads.push_back(std::move(t));
  }

  // ---- 创建客户端 ----
  InetAddress serverAddr(args.host, args.port);
  LatencyStats stats;
  std::vector<std::unique_ptr<BenchClient>> clients;

  for (int i = 0; i < args.clients; ++i) {
    EventLoop *loop = loops[i % loops.size()]; // round-robin
    auto client = std::make_unique<BenchClient>(loop, serverAddr, &stats, i);
    client->connect();
    clients.push_back(std::move(client));
  }
  CurrentThread::sleepUsec(500 * 1000); // 等连接建立

  // 统计连接数
  int connected = 0;
  for (auto &c : clients)
    if (c->connected()) connected++;
  std::cout << "Connected: " << connected << "/" << args.clients << "\n\n";

  // ---- 预热 ----
  std::cout << "--- Warmup (2s) ---\n";
  {
    std::atomic<bool> stopWarmup{false};
    auto warmupFunc = [&](BenchClient *c, int seed) {
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> dist(0, 999);
      while (!stopWarmup.load(std::memory_order_relaxed)) {
        std::string key = "warmup_" + std::to_string(dist(rng));
        c->sendAndWait("SET " + key + " val\r\n");
      }
    };

    std::vector<std::thread> warmupThreads;
    for (int i = 0; i < args.clients; ++i) {
      warmupThreads.emplace_back(warmupFunc, clients[i].get(), i * 37 + 42);
    }
    CurrentThread::sleepUsec(2 * 1000 * 1000);
    stopWarmup.store(true);
    for (auto &t : warmupThreads)
      t.join();
  }
  std::cout << "Warmup done.\n\n";

  // ---- 基准测试 ----
  std::cout << "--- Benchmark (" << args.durationSec << "s) ---\n";

  AtomicInt64 totalOps;
  AtomicInt64 totalErrors;
  std::atomic<bool> stopBench{false};

  auto benchFunc = [&](BenchClient *c, int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> keyDist(0, 99999);
    std::uniform_int_distribution<int> opDist(0, 9);
    // 80% GET, 20% SET (模拟读多写少)

    while (!stopBench.load(std::memory_order_relaxed)) {
      std::string key = "bench_" + std::to_string(keyDist(rng));

      if (opDist(rng) < 8) {
        c->sendAndWait("GET " + key + "\r\n");
      } else {
        c->sendAndWait(
            "SET " + key + " value_" + std::to_string(keyDist(rng)) + "\r\n");
      }
      totalOps.increment();
    }
  };

  std::vector<std::thread> benchThreads;
  for (int i = 0; i < args.clients; ++i) {
    benchThreads.emplace_back(benchFunc, clients[i].get(), i * 131 + 7);
  }

  // 每秒报告进度
  int64_t lastOps = 0;
  for (int sec = 1; sec <= args.durationSec; ++sec) {
    CurrentThread::sleepUsec(1000 * 1000);
    int64_t current = totalOps.get();
    int64_t qps = current - lastOps;
    lastOps = current;
    std::cout << "  [" << sec << "s] QPS: " << qps
              << "  Total: " << current << "\n";
  }

  stopBench.store(true);
  for (auto &t : benchThreads)
    t.join();

  // ---- 结果 ----
  std::cout << "\n===== Results =====\n";
  std::cout << "Total ops     : " << totalOps.get() << "\n";
  std::cout << "Duration      : " << args.durationSec << "s\n";
  double avgQps = static_cast<double>(totalOps.get()) / args.durationSec;
  std::cout << "Average QPS   : " << static_cast<int64_t>(avgQps) << "\n";
  std::cout << "QPS / client  : " << static_cast<int64_t>(avgQps / args.clients)
            << "\n\n";

  std::cout << "--- Latency Distribution ---\n";
  stats.print();

  // 清理
  for (auto &c : clients) {
    c->connect(); // disconnect (actually we just let them destruct)
  }

  return 0;
}
