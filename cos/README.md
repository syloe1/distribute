# COS Object Storage - 分布式对象存储系统（第一阶段）

一个基于 Go + gRPC + LevelDB 实现的轻量级分布式对象存储系统，核心支持大文件分片上传/下载、分片存储与索引管理，专为解决大文件传输与存储问题设计。

## 项目背景
采用「分片上传+分布式存储」架构，第一阶段已完成基础通信、分片存储、客户端上传下载全流程搭建，为后续功能扩展奠定基础。

## 核心功能（第一阶段）
✅ 基础架构通信：基于gRPC实现客户端-网关-ChunkServer的RPC通信  
✅ ChunkServer存储：分片数据本地文件存储 + LevelDB索引管理（分片ID→存储路径映射）  
✅ 客户端工具：支持文件分片上传、分片合并下载  
✅ 流式传输：大文件分片流式上传/下载，降低内存占用  

## 技术栈
- 开发语言：Go 1.24+
- 通信框架：gRPC（高性能RPC通信）
- 存储方案：LevelDB（轻量级键值数据库，存储分片索引）
- 工具依赖：protobuf（数据序列化）、goleveldb（LevelDB Go客户端）

## 环境准备
### 1. 依赖安装
```bash
# 1. 安装Go（版本≥1.24）：https://go.dev/dl/
# 2. 安装protobuf编译器（protoc）：https://grpc.io/docs/protoc-installation/
# 3. 安装gRPC Go插件
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
# 4. 安装项目依赖
go mod tidy
```

## 项目结构
```
cos/
├── cmd/
│   ├── chunkserver/  # ChunkServer服务（分片存储）
│   └── client/       # 客户端工具（上传/下载）
├── internal/
│   ├── chunkserver/  # ChunkServer核心逻辑
│   └── proto/        # gRPC协议定义与生成代码
├── pkg/
│   └── utils/        # 工具函数（文件分片/合并）
├── go.mod            # 项目依赖管理
└── README.md         # 项目说明
```

## 快速开始
### 1. 启动 ChunkServer（分片存储服务）
```bash
# 进入项目根目录
cd cos/
# 启动ChunkServer（默认监听50051端口，存储路径：./leveldb_data ./chunk_data）
go run cmd/chunkserver/main.go
```

### 2. 启动 Gateway（可选，若已实现）
```bash
# 若Gateway单独部署，启动命令（示例）
go run cmd/gateway/main.go
```

### 3. 客户端上传文件
```bash
# 格式：go run cmd/client/main.go -op upload -path 本地文件路径
go run cmd/client/main.go -op upload -path ./test.txt
```
成功后会输出FileID（用于下载），示例：`upload success! FileID: 7907a97a79b6b47895818295ed79a18c`

### 4. 客户端下载文件
```bash
# 格式：go run cmd/client/main.go -op download -fileid 上传时的FileID -path 保存路径
go run cmd/client/main.go -op download -fileid 7907a97a79b6b47895818295ed79a18c -path ./downloaded_test.txt
```

## 存储说明
- 分片文件存储：默认路径`./chunk_data/{FileID}/{ChunkID}`（可在 ChunkServer 启动代码中修改`chunkStorePath`）
- 索引存储：LevelDB 数据库路径`./leveldb_data`（存储分片 ID 与文件路径的映射）

## 后续规划（第二阶段）
1. 完善 Gateway 路由功能，支持多 ChunkServer 集群
2. 增加文件元数据管理（文件名、大小、上传时间）
3. 实现分片校验（MD5 校验，避免传输损坏）
4. 增加权限控制（API 密钥、访问白名单）
5. 支持断点续传

## 作者
GitHub: syloe1