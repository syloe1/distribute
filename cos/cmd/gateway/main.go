package main

import (
	"context"
	"log"
	"net"
	// "strings"
	"fmt"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"github.com/syloe1/cos/internal/proto"  // 替换syloe1
)

const (
	gatewayAddr = ":50052"  // Gateway监听端口
	chunkServerAddr = "localhost:50051"  // ChunkServer地址（需与ChunkServer一致）
)

// Gateway 结构体（持有ChunkServer客户端）
type Gateway struct {
	proto.UnimplementedObjectServiceServer  // 嵌入接口
	chunkClient proto.ChunkServiceClient    // ChunkServer客户端
}

// NewGateway 创建Gateway实例（连接ChunkServer）
func NewGateway(chunkServerAddr string) (*Gateway, error) {
	// 连接ChunkServer（开发阶段用insecure，生产需加TLS）
	conn, err := grpc.Dial(chunkServerAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("connect chunk server failed: %v", err)
	}

	return &Gateway{
		chunkClient: proto.NewChunkServiceClient(conn),
	}, nil
}

// UploadFile 实现Client→Gateway的上传接口：接收分块→转发给ChunkServer
func (g *Gateway) UploadFile(stream proto.ObjectService_UploadFileServer) error {
	var fileID string
	var totalChunks int32

	// 1. 接收Client的流式分块
	for {
		chunk, err := stream.Recv()
		if err != nil {
			// 接收完毕，返回响应
			return stream.SendAndClose(&proto.UploadFileResponse{
				Success: true,
				FileId:  fileID,
				Message: fmt.Sprintf("file %s uploaded success (total chunks: %d)", fileID, totalChunks),
			})
		}

		// 记录文件ID和总块数（从第一个块获取）
		if fileID == "" {
			fileID = chunk.FileId
			totalChunks = chunk.TotalChunks
		}

		// 2. 转发块到ChunkServer
		_, err = g.chunkClient.StoreChunk(context.Background(), chunk)
		if err != nil {
			log.Printf("store chunk %s failed: %v", chunk.ChunkId, err)
			return fmt.Errorf("upload failed: %v", err)
		}
		log.Printf("forwarded chunk %s (index: %d/%d)", chunk.ChunkId, chunk.ChunkIndex, chunk.TotalChunks)
	}
}

// DownloadFile 实现Client→Gateway的下载接口：从ChunkServer获取块→转发给Client
func (g *Gateway) DownloadFile(req *proto.DownloadFileRequest, stream proto.ObjectService_DownloadFileServer) error {
	// 注意：这里简化实现（实际需先从MetaNode获取该文件的所有块ID）
	// 临时方案：假设已知块ID格式为"fileID_chunkIndex"，总块数需从MetaNode获取（这里先写死为2，后续优化）
	totalChunks := 2  // 实际场景需从MetaNode查询，这里仅做测试
	for i := 0; i < totalChunks; i++ {
		chunkID := fmt.Sprintf("%s_chunk%d", req.FileId, i)

		// 1. 从ChunkServer获取块
		chunkResp, err := g.chunkClient.GetChunk(context.Background(), &proto.GetChunkRequest{
			FileId:  req.FileId,
			ChunkId: chunkID,
		})
		if err != nil {
			log.Printf("get chunk %s failed: %v", chunkID, err)
			return err
		}

		// 2. 转发块给Client
		if err := stream.Send(chunkResp); err != nil {
			log.Printf("send chunk %s to client failed: %v", chunkID, err)
			return err
		}
		log.Printf("sent chunk %s to client", chunkID)
	}

	return nil
}

func main() {
	// 1. 创建Gateway实例
	gateway, err := NewGateway(chunkServerAddr)
	if err != nil {
		log.Fatalf("create gateway failed: %v", err)
	}

	// 2. 启动Gateway的gRPC服务
	lis, err := net.Listen("tcp", gatewayAddr)
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}
	s := grpc.NewServer()
	proto.RegisterObjectServiceServer(s, gateway)  // 注册ObjectService

	log.Printf("gateway listening at %v", lis.Addr())
	if err := s.Serve(lis); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}