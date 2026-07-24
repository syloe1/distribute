# Distributed KV

基于 [Muduo](https://github.com/chenshuo/muduo) 网络库实现的高性能分布式 KV 存储，使用 C++17 编写。通过一致性哈希做数据分片、分片锁提升并发、异步主从复制保证容错。

## 架构总览

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  KVServer   │     │  KVServer   │     │  KVServer   │
│  (node-1)   │◄───►│  (node-2)   │◄───►│  (node-3)   │
│  :7001      │     │  :7002      │     │  :7003      │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       └───────────────────┼───────────────────┘
                           │
                    ┌──────┴──────┐
                    │   KVClient   │
                    │ (REPL / CLI) │
                    └─────────────┘
```

### 模块说明

| 模块 | 文件 | 描述 |
|--------|------|------|
| **KVStore** | [src/kv_store.h](src/kv_store.h) | 分片锁内存存储引擎，16 个分片各自持有一把 mutex。每个分片内部是 `std::unordered_map`，O(1) 读写；不同分片的 key 可完全并发访问。 |
| **HashRing** | [src/hash_ring.h](src/hash_ring.h) | 一致性哈希环，每个物理节点映射 150 个虚拟节点。用 MD5 做哈希，`std::map::upper_bound` 做 O(log N·V) 查找。 |
| **Protocol** | [src/common.h](src/common.h) | RESP 风格的文本协议。以行为单位（`\r\n` 分隔），支持 SET、GET、DEL、PING 四条指令。 |
| **Replicator** | [src/replicator.h](src/replicator.h) | 异步主从复制，星型拓扑。fire-and-forget 语义追求高吞吐；从节点断开后自动重连。 |
| **Server** | [src/kv_server.cpp](src/kv_server.cpp) | 基于 Muduo TcpServer 的多线程服务端，接收连接、解析命令、分发到 KVStore + Replicator。 |
| **Client** | [src/kv_client.cpp](src/kv_client.cpp) | 命令行客户端，一致性哈希路由，每个节点一个长连接，REPL 交互界面。主线程与 I/O 线程通过 `promise/future` 通信。 |

### 数据流

```
客户端 REPL → 解析命令 → hash(key) → 在哈希环上定位节点 → TCP 发送
                                                              │
服务端接收 ← muduo TcpServer ← 协议解析 ← 分发               │
       │                                                     │
       ├── SET/DEL: 写本地 KVStore → 异步复制到从节点 ───────┘
       └── GET:    读 KVStore → 返回 RESP 响应 ──────────────┘
```

## 快速开始

### 环境依赖

- C++17 编译器（GCC 8+ / Clang 10+）
- CMake 3.16+
- [Muduo](https://github.com/chenshuo/muduo) 安装到 `~/.local`（或自行修改 [CMakeLists.txt](CMakeLists.txt) 中的 `MUDUO_PREFIX`）
- OpenSSL 开发头文件（`libssl-dev`）
- pthreads

### 编译

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 启动集群

在三个终端分别启动服务节点：

```bash
# 终端 1 — 主节点，配置复制到 node-2 和 node-3
./build/kv_server --id node-1 --port 7001 --replicas node-2:7002,node-3:7003

# 终端 2 — 从节点
./build/kv_server --id node-2 --port 7002

# 终端 3 — 从节点
./build/kv_server --id node-3 --port 7003
```

启动客户端连接集群：

```bash
./build/kv_client 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003
```

### 客户端命令

```
SET <key> <value>   写入键值对
GET <key>           读取键
DEL <key>           删除键
NODES               查看集群拓扑
QUIT                退出
```

## 设计要点

### 一致性模型

在 CAP 定理中，本系统选择 **AP**（可用性 + 分区容忍），放弃 **C**（强一致性）：
- 写操作在本地提交后立即返回 `+OK`，不等复制完成。
- 复制是异步 fire-and-forget —— 从节点可能落后于主节点。
- 牺牲线性一致性，换取更高的写入吞吐和更低的延迟。

### 分片锁

KVStore 不使用全局大锁，而是拆成 16 个独立分片，每个分片一把 mutex。key 通过 `hash(key) % 16` 映射到分片。不同分片的 key 完全无锁竞争，16 路并发读写。

### 一致性哈希 + 虚拟节点

每个物理节点在哈希环上对应 150 个虚拟节点，平滑数据分布 —— 3 节点场景下分布标准差通常在 5%~10%。增减节点时只需迁移约 `1/N` 的 key，避免全量重分布。

### 线程模型

- **服务端**：Muduo 多 Reactor 模型 —— 1 个 acceptor 线程 + N 个 I/O 线程。KVStore 跨线程共享，分片锁保证低竞争。
- **客户端**：1 个 I/O 线程（Muduo EventLoop）处理所有网络读写；主线程跑 REPL，通过 `std::promise/future` 与 I/O 线程同步。

## 未来可扩展方向

### 高优先级

- **Raft 共识协议**：用 Raft 替换当前的主从异步复制，获得强一致性（线性一致性）、自动选主、安全故障转移。C++ 生态可参考 [braft](https://github.com/baidu/braft) 或 [libraft](https://github.com/canonical/raft)。

- **持久化层**：增加可选的磁盘持久化 —— 快照（定期全量 dump）+ WAL（增量日志）。可引入 RocksDB 或 LevelDB 替换内存中的 `std::unordered_map`，保留分片架构不变。

- **HTTP / Redis 协议兼容**：增加协议适配层，使服务端同时支持 Redis 协议（RESP2/RESP3）或 RESTful HTTP API（`GET /api/v1/{key}`、`PUT /api/v1/{key}`），让标准 Redis 客户端或 `curl` 直接访问集群。

- **服务发现**：用 Etcd、Consul 或 ZooKeeper 替换客户端中的静态节点列表。节点启动时注册、退出时注销；客户端监听成员变更事件，自动重建哈希环。

- **TLS / mTLS**：基于已链接的 OpenSSL 增加传输层加密。mTLS 可实现服务端之间、客户端与服务端之间的双向认证 —— 多数据中心部署的必备能力。

### 中优先级

- **Read Repair 与 Hinted Handoff**：从节点读到过期或缺失数据时，触发从主节点的 read-repair。从节点短暂下线期间，主节点缓冲写操作，待其重连后回放（hinted handoff）。

- **数据过期（TTL）**：支持 `SETEX key seconds value` 和 `EXPIRE key seconds`。采用惰性过期（访问时检查）+ 定期后台抽样清理（类似 Redis 的做法）。

- **可观测性**：暴露 Prometheus 指标（QPS、延迟分位数、错误率、复制延迟、分片大小）。增加结构化 JSON 日志和请求追踪 ID，方便排查分布式问题。

- **客户端缓存**：客户端侧实现本地 LRU 缓存，服务端推送失效消息。对热点 key 可大幅减少网络往返。

- **批量操作**：支持 `MGET`、`MSET`，一次往返读写多个 key。客户端按目标节点分组 key，并发发出请求后合并结果。

- **命名空间 / 多租户**：增加逻辑数据库编号（`SELECT n`）或 key 前缀机制，隔离共享同一集群的不同应用。

### 低优先级

- **gRPC 流式复制**：用 gRPC 双向流替换自定义 TCP 复制协议，获得内置的流控、背压、以及在同一连接上多路复用不同复制流（快照 + 增量）的能力。

- **压缩**：对超过阈值的 value 用 Snappy / LZ4 / Zstd 透明压缩，尤其适合 JSON 或大 blob 场景。

- **二级索引**：支持对 value 字段建索引并查询。例如按 email 索引用户记录，无需知道主键即可 O(1) 查找。

- **事务**：实现多 key 事务，跨哈希环节点采用乐观并发控制（OCC）或两阶段锁（2PL），由客户端或事务协调器统一协调。

- **Bloom Filter / Cuckoo Filter**：在引入持久化后，用概率数据结构快速应答"key 不存在"的查询，减少无效的磁盘读取。

- **异地多活**：支持多数据中心部署，可配置复制拓扑（mesh / star / tree）和按 key 粒度的复制策略（如复制到 2 个 DC，或按合规要求数据不出特定区域）。

- **热点 Key 治理**：自动检测热点 key，增加服务端读缓存，或将热点 key 复制到多个节点分摊读压力。

- **Lua 脚本**：嵌入 Lua（或其他轻量运行时），支持服务端原子执行脚本，类似 Redis 的 `EVAL`。可用于实现 CAS、限流器、复合操作。

- **WASM 插件系统**：允许用户以 WebAssembly 模块编写自定义命令处理器或数据变换逻辑，沙箱隔离安全运行，支持热加载。

## License

MIT
