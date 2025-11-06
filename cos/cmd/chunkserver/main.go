package main

import (
	"log"
	"net"

	"google.golang.org/grpc"
	"github.com/syloe1/cos/internal/chunkserver"  // 替换syloe1
	"github.com/syloe1/cos/internal/proto"
)

const (
	addr = ":50051"  // ChunkServer监听端口（可自定义）
	dbPath = "./data/leveldb"  // LevelDB存储路径
	chunkStorePath = "./data/chunks"  // 块文件存储路径
)

func main() {
	// 1. 创建ChunkServer实例
	cs, err := chunkserver.NewChunkServer(dbPath, chunkStorePath)
	if err != nil {
		log.Fatalf("create chunk server failed: %v", err)
	}
	defer cs.Close()  // 程序退出时关闭LevelDB

	// 2. 启动gRPC服务
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}
	s := grpc.NewServer()
	proto.RegisterChunkServiceServer(s, cs)  // 注册ChunkService

	log.Printf("chunk server listening at %v", lis.Addr())
	if err := s.Serve(lis); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}