# 分布式 KV 存储 — 学习指南

## 项目概述

这是一个用 **muduo 网络库** 实现的分布式键值存储系统，约 1000 行 C++ 代码。对标 Redis 的基础功能，展示如何在 muduo 的 **事件驱动 + 回调** 范式下构建分布式系统。

### 学习目标

读完这个项目，你将掌握：

| 技能 | 对应文件 |
|------|----------|
| muduo TcpServer / TcpClient 用法 | `kv_server.cpp`, `kv_client.cpp` |
| TCP 流式协议解析（半包/粘包处理） | `common.h` |
| 一致性哈希算法实现 | `hash_ring.h` |
| 多线程安全存储（分片锁） | `kv_store.h` |
| 异步主从复制 | `replicator.h` |
| C++ 事件驱动编程模式 | 全部文件 |

---

## 前置知识

在开始之前，确保你理解：

1. **TCP 流式特性**：TCP 是字节流，没有消息边界。你需要自己定义"一条完整消息"是什么。
2. **Reactor 模式**：一个线程循环等待事件（I/O、定时器），事件到达时调用回调函数。muduo 的核心就是 `EventLoop`。
3. **Go 开发者注意**：Go 的 goroutine + channel 让并发"看起来同步"，muduo 是纯回调 —— 你注册回调，框架在 I/O 线程里调用它们。不要在回调里阻塞！

### 环境准备

```bash
# muduo 已安装在 ~/.local, 无需额外操作
cd /home/wk/distributed-kv/build
cmake .. && make -j$(nproc)
```

---

## 架构全景图

```
                         ┌─────────────────────┐
                         │    kv_client.cpp     │
                         │   ┌───────────────┐  │
   用户输入 ──────────►  │   │  REPL 循环    │  │ (主线程)
   "SET foo bar"         │   └───┬───────────┘  │
                         │       │ syncSET()    │
                         │   ┌───▼───────────┐  │
                         │   │  HashRing     │  │ hash("foo") → 定位节点
                         │   └───┬───────────┘  │
                         │   ┌───▼───────────┐  │
                         │   │ TcpClient × N │  │ (EventLoop 线程)
                         │   └───┬───────────┘  │
                         └───────┼──────────────┘
                                 │ TCP
        ┌────────────────────────┼────────────────────────┐
        │                        │                        │
   ┌────▼─────┐           ┌──────▼──────┐          ┌──────▼──────┐
   │ 节点1    │           │  节点2      │          │  节点3      │
   │ :7001    │           │  :7002      │          │  :7003      │
   │          │           │             │          │             │
   │ TcpServer│           │ TcpServer   │          │ TcpServer   │
   │    │     │           │    │        │          │    │        │
   │ onMessage│           │ onMessage   │          │ onMessage   │
   │    │     │           │    │        │          │    │        │
   │ Protocol │           │ Protocol    │          │ Protocol    │
   │ .parse() │           │ .parse()    │          │ .parse()    │
   │    │     │           │    │        │          │    │        │
   │ KVStore │           │ KVStore     │          │ KVStore     │
   │ (16shard)│          │ (16shard)   │          │ (16shard)   │
   │    │     │           │             │          │             │
   │Replicator│(可选)     │             │          │             │
   │ 异步复制  │──────────►  (从节点接收复制数据)   │             │
   └──────────┘           └─────────────┘          └─────────────┘
```

---

## 阅读顺序（由浅入深）

### 第一步：理解数据结构 (`common.h` + `kv_store.h`)

**先看 `common.h`** — 这是最基础的文件，定义了整个系统共享的类型：

```
Node          → 集群中的一个物理节点 (id, host, port)
Command       → 枚举: SET / GET / DEL / PING / UNKNOWN
ParsedCommand → 解析后的命令 {type, key, value}
Protocol      → 协议解析器 (静态方法)
```

**关键代码——协议解析：**

```cpp
static bool parseCommand(Buffer* buf, ParsedCommand* cmd) {
    const char* crlf = buf->findCRLF();  // 在 Buffer 中找 \r\n
    if (!crlf) return false;             // 半包 → 等待更多数据

    std::string line(buf->peek(), crlf - buf->peek());
    buf->retrieveUntil(crlf + 2);        // 消费已解析的数据
    // ... 按空格分割, 识别命令 ...
}
```

这是 TCP 编程的核心问题——**消息边界**。TCP 只保证字节顺序，不保证 `send("SET a b\r\n")` 到对端时是一次 `recv` 收到整条消息。解决方案：用 `\r\n` 作为消息分隔符，`findCRLF()` 扫 Buffer，找不到就 return false 等下次回调。

**再看 `kv_store.h`** — 理解分片锁：

```cpp
class KVStore {
    static constexpr size_t NUM_SHARDS = 16;
    struct Shard {
        unordered_map<string, string> data;
        mutable MutexLock mutex;      // 每个 Shard 一把锁
    };
    Shard shards_[16];
};
```

| 方案 | 并发度 | 实现 |
|------|--------|------|
| 全局锁 | 1 | `mutex.lock(); map[key] = val; mutex.unlock();` |
| 分片锁 | 16 | `shard = hash(key) % 16; shard.mutex.lock(); ...` |

16 个 shard 意味着 16 个不同 shard 上的 key 可以同时读写。俩 key 冲突概率 = 1/16。

### 第二步：理解一致性哈希 (`hash_ring.h`)

核心问题：3 个节点，每个 key 应该存在哪个节点？

**朴素方案** `hash(key) % 3`：加一个节点，几乎所有 key 都要迁移！
**一致性哈希**：把节点和 key 都映射到同一个环上，key 归属于顺时针最近的节点。加节点只影响相邻的一小段。

```
虚拟节点环:

    hash=0  ──────────────────── hash=2^64
       │                              │
       │    VN_A_2  VN_B_0            │
       │      │       │               │
       │      └───●───┘               │
       │    key "foo" 映射到这里      │
       │    顺时针遇到 VN_A_2 → 节点A │
       │                              │
```

150 个虚拟节点/物理节点，是为了让 key 分布更加均匀。

### 第三步：理解服务器 (`kv_server.cpp`)

核心是 muduo 的回调注册模式：

```cpp
// 1. 创建 TcpServer
TcpServer server_(loop, listenAddr, "KVServer");

// 2. 设置回调——这是 muduo 的核心模式
server_.setConnectionCallback(onConnection);  // 连接建立/断开时调用
server_.setMessageCallback(onMessage);        // 数据到达时调用

// 3. 启动
server_.start();
loop.loop();  // 阻塞, 进入事件循环
```

**完整的请求处理流程：**

```
TCP 数据到达
  → muduo 内核把数据读到 Buffer
  → 调用 onMessage(conn, buf, timestamp)
  → while (buf 里有完整命令) {
        Protocol::parseCommand(buf, &cmd)   // 解析
        response = CommandHandler.handle(cmd) // 处理
        conn->send(response)                 // 响应
    }
  → 如果 buf 里剩半条命令, 退出循环, 等下次回调
```

**Go 对比：**
```go
// Go — 同步风格, 每个连接一个 goroutine
func handle(conn net.Conn) {
    scanner := bufio.NewScanner(conn)
    for scanner.Scan() {
        response := process(scanner.Text())
        conn.Write([]byte(response))
    }
}
```

```cpp
// muduo — 回调风格, 所有连接共享 I/O 线程
server_.setMessageCallback([](auto conn, auto buf, auto time) {
    while (buf->readableBytes() > 0) {
        // 解析 + 处理 + 响应
    }
});
```

### 第四步：理解客户端 (`kv_client.cpp`)

客户端有两个关键设计：

**1. 一致性哈希路由**

```cpp
string syncSET(const string& key, const string& value) {
    const Node* node = ring_.getNode(key);     // 哈希定位
    return sendToNode(*node, "SET " + key + ...); // 发给目标节点
}
```

**2. 桥接 REPL 线程和 I/O 线程**

```
主线程 (REPL)              I/O 线程 (网络)
    │                          │
    ├─ sendToNode()            │
    │  ├─ 创建 promise ─────────► runInLoop() 发送命令
    │  ├─ future.wait_for(5s)  │  ...
    │  │                       ├─ onMessage() 收到响应
    │  │                       ├─ promise.set_value(resp)
    │  ◄────────────────────────┘
    ├─ return future.get()
```

muduo 是单线程事件循环，网络操作必须在 I/O 线程执行。主线程用 `loop_->runInLoop()` 把任务投递过去，用 `std::promise/future` 等待结果。

### 第五步：理解复制 (`replicator.h`)

```
客户端 ──SET key val──► 主节点 ──写入本地 KVStore──► 返回 +OK
                            │
                            ├──► 异步发送 SET key val ──► 从节点1
                            └──► 异步发送 SET key val ──► 从节点2
```

这是一个 **AP 系统**（CAP 定理）：
- 可用性（Available）: 主节点立即响应，不等从节点
- 分区容忍（Partition tolerant）: 从节点挂了不影响主节点
- 牺牲一致性（Consistency）: 从节点可能丢失数据

异步复制是 fire-and-forget：主节点 `conn->send(cmd)` 后立即返回，不检查从节点是否收到。丢失的复制数据不会重传。

---

## muduo 核心 API 速查

| muduo 类 | 作用 | 关键方法 |
|----------|------|---------|
| `EventLoop` | 事件循环 | `loop()`, `runInLoop(fn)` |
| `TcpServer` | 服务端 | `setConnectionCallback()`, `setMessageCallback()`, `start()` |
| `TcpClient` | 客户端 | `connect()`, `disconnect()`, `enableRetry()` |
| `TcpConnection` | 一条连接 | `send(data)`, `shutdown()`, `connected()` |
| `Buffer` | 读写缓冲 | `findCRLF()`, `peek()`, `retrieve()`, `append()` |
| `MutexLock` | 互斥锁 | `lock()`, `unlock()` |
| `MutexLockGuard` | RAII 锁 | 构造时 lock, 析构时 unlock |

---

## 常见问题

### Q: 为什么 muduo 用回调而不是同步读写？

同步读写需要每个连接独占一个线程。10000 个连接 = 10000 个线程 = 很大的上下文切换开销。muduo 用少量 I/O 线程（通常等于 CPU 核数）+ 非阻塞 I/O + 事件通知，4 个线程就能处理 10000 个连接。

### Q: TcpConnection::send() 是异步的吗？

是的。`send()` 把数据放到输出缓冲区，立即返回。muduo 在后台把数据写给内核。如果你想在数据写完时得到通知，注册 `WriteCompleteCallback`。

### Q: 为什么 KVStore 要用分片锁而不是读写锁？

读写锁（shared_mutex）在高竞争时有性能问题（需要原子操作修改 reader counter）。分片锁把热点数据分散到多个锁上，每个锁的竞争概率降低。16 个分片 = 写竞争概率降低到 1/16。

### Q: findCRLF 找不到 \r\n 怎么办？

返回 false, Buffer 保留不完整的数据。等下次 TCP 数据到达，muduo 自动追加到 Buffer 后面，再次回调 onMessage，继续解析。

### Q: 一致性哈希用 MD5, 性能如何？

MD5 非常快（~400 MB/s 单核），对于 KV 存储的场景完全不是瓶颈。瓶颈在网络 I/O 和存储操作。

---

## 扩展方向（由浅入深）

### Level 1: 完善基础功能

- [ ] **KEYS 命令**: 返回所有 key 的数量
- [ ] **EXPIRE 支持**: key 带 TTL, 到期自动删除
- [ ] **错误处理**: 键/值长度限制, 防止 OOM
- [ ] **连接数限制**: 防止过多客户端连接

### Level 2: 增强分布式能力

- [ ] **WAL 日志**: 写前日志, 节点重启后恢复数据
- [ ] **节点发现**: 把硬编码的节点列表换成 gossip 协议
- [ ] **Hinted Handoff**: 从节点恢复后, 主节点补发错过的写入
- [ ] **Read Repair**: 读时检测各副本不一致并修复

### Level 3: 进阶

- [ ] **Raft 共识**: 替换异步复制, 实现强一致性
- [ ] **Protobuf 协议**: 用 Protobuf 替代文本协议, 提升性能
- [ ] **LSM 存储引擎**: 把 `unordered_map` 换成持久化的 LSM Tree
- [ ] **事务支持**: MULTI/EXEC 风格的事务

---

## 调试技巧

```bash
# 1. 用 telnet 直接测试协议
telnet 127.0.0.1 7001
PING            # 返回 +PONG
SET a b         # 返回 +OK
GET a           # 返回 $1 b

# 2. 查看 muduo 日志 (调整日志级别)
# 在 main() 中:
Logger::setLogLevel(Logger::DEBUG);  # 看所有网络事件

# 3. 用 strace 追踪系统调用
strace -e trace=network ./kv_server --id n1 --port 7001

# 4. 运行自动化测试
cd /home/wk/distributed-kv
./test.sh               # 完整测试
./test.sh --quick       # 快速模式
./test.sh --verbose     # 详细输出
```

---

## 推荐阅读

1. **《Linux 多线程服务端编程》** — 陈硕 (muduo 作者), 第 6-8 章讲 muduo 设计
2. **muduo 源码 examples/** — `examples/hub/` 是聊天室, `examples/memcached/` 是 memcache 协议
3. **Redis Cluster Specification** — 学习一致性哈希在生产环境的应用
4. **《Designing Data-Intensive Applications》** — 第 5-6 章, 复制与分片

---

## 一句话总结

> muduo 帮你处理了 epoll、非阻塞 I/O、线程池这些底层细节。你只需注册回调函数，在回调里解析协议、操作数据、发送响应。分布式的部分——哈希分片、数据复制——和 muduo 无关，是你自己的应用逻辑。
