package chunkserver

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"context"
	"github.com/syndtr/goleveldb/leveldb"
	"github.com/syloe1/cos/internal/proto"  // 替换syloe1为你的模块名
)

// ChunkServer 结构体（存储配置和LevelDB实例）
type ChunkServer struct {
	proto.UnimplementedChunkServiceServer  // 必须嵌入，实现gRPC接口
	dbPath       string  // LevelDB存储路径
	chunkStorePath string// 块文件本地存储路径
	db           *leveldb.DB  // LevelDB实例
}

// NewChunkServer 创建ChunkServer实例
func NewChunkServer(dbPath, chunkStorePath string) (*ChunkServer, error) {
	// 1. 创建存储目录（若不存在）
	if err := os.MkdirAll(dbPath, 0755); err != nil {
		return nil, fmt.Errorf("create db dir failed: %v", err)
	}
	if err := os.MkdirAll(chunkStorePath, 0755); err != nil {
		return nil, fmt.Errorf("create chunk dir failed: %v", err)
	}

	// 2. 打开LevelDB
	db, err := leveldb.OpenFile(dbPath, nil)
	if err != nil {
		return nil, fmt.Errorf("open leveldb failed: %v", err)
	}

	return &ChunkServer{
		dbPath:         dbPath,
		chunkStorePath: chunkStorePath,
		db:             db,
	}, nil
}

// StoreChunk 实现gRPC的StoreChunk接口：存储块到本地+LevelDB记录索引
// func (s *ChunkServer) StoreChunk(req *proto.ChunkInfo, stream proto.ChunkService_StoreChunkServer) error {
// 	// 1. 生成块文件本地路径（chunkStorePath/file_id/chunk_id）
// 	chunkDir := filepath.Join(s.chunkStorePath, req.FileId)
// 	if err := os.MkdirAll(chunkDir, 0755); err != nil {
// 		return fmt.Errorf("create chunk dir %s failed: %v", chunkDir, err)
// 	}
// 	chunkPath := filepath.Join(chunkDir, req.ChunkId)

// 	// 2. 写入块数据到本地文件
// 	if err := os.WriteFile(chunkPath, req.Data, 0644); err != nil {
// 		return fmt.Errorf("write chunk %s failed: %v", chunkPath, err)
// 	}

// 	// 3. LevelDB记录：key=chunk_id, value=chunkPath
// 	if err := s.db.Put([]byte(req.ChunkId), []byte(chunkPath), nil); err != nil {
// 		// 回滚：删除已写入的块文件
// 		os.Remove(chunkPath)
// 		return fmt.Errorf("write leveldb failed: %v", err)
// 	}

// 	// 4. 返回成功响应
// 	return stream.SendAndClose(&proto.StoreChunkResponse{
// 		Success: true,
// 		Message: fmt.Sprintf("chunk %s stored success", req.ChunkId),
// 	})
// }
func (s *ChunkServer) StoreChunk(ctx context.Context, req *proto.ChunkInfo) (*proto.StoreChunkResponse, error) {
	// 业务逻辑（创建目录、写文件、LevelDB 记录）完全不变
	chunkDir := filepath.Join(s.chunkStorePath, req.FileId)
	if err := os.MkdirAll(chunkDir, 0755); err != nil {
		return nil, fmt.Errorf("create chunk dir %s failed: %v", chunkDir, err)
	}
	chunkPath := filepath.Join(chunkDir, req.ChunkId)

	if err := os.WriteFile(chunkPath, req.Data, 0644); err != nil {
		return nil, fmt.Errorf("write chunk %s failed: %v", chunkPath, err)
	}

	if err := s.db.Put([]byte(req.ChunkId), []byte(chunkPath), nil); err != nil {
		os.Remove(chunkPath)
		return nil, fmt.Errorf("write leveldb failed: %v", err)
	}

	// 🔥 修复：直接返回响应（不用 stream.SendAndClose）
	return &proto.StoreChunkResponse{
		Success: true,
		Message: fmt.Sprintf("chunk %s stored success", req.ChunkId),
	}, nil
}
// GetChunk 实现gRPC的GetChunk接口：从LevelDB查路径→读取块数据
// func (s *ChunkServer) GetChunk(req *proto.GetChunkRequest, stream proto.ChunkService_GetChunkServer) error {
// 	// 1. LevelDB查询块路径
// 	chunkPathBytes, err := s.db.Get([]byte(req.ChunkId), nil)
// 	if err != nil {
// 		if err == leveldb.ErrNotFound {
// 			return errors.New("chunk not found")
// 		}
// 		return fmt.Errorf("read leveldb failed: %v", err)
// 	}
// 	chunkPath := string(chunkPathBytes)

// 	// 2. 读取本地块文件
// 	data, err := os.ReadFile(chunkPath)
// 	if err != nil {
// 		return fmt.Errorf("read chunk %s failed: %v", chunkPath, err)
// 	}

// 	// 3. 返回块信息（ChunkInfo需补充必要字段，这里简化）
// 	return stream.SendAndClose(&proto.ChunkInfo{
// 		FileId:     req.FileId,
// 		ChunkId:    req.ChunkId,
// 		Data:       data,
// 		ChunkIndex: 0,  // 实际场景需从LevelDB或文件中读取，这里简化
// 		TotalChunks: 0,
// 	})
// }
func (s *ChunkServer) GetChunk(ctx context.Context, req *proto.GetChunkRequest) (*proto.ChunkInfo, error) {
	// 业务逻辑完全不变
	chunkPathBytes, err := s.db.Get([]byte(req.ChunkId), nil)
	if err != nil {
		if err == leveldb.ErrNotFound {
			return nil, errors.New("chunk not found")
		}
		return nil, fmt.Errorf("read leveldb failed: %v", err)
	}
	chunkPath := string(chunkPathBytes)

	data, err := os.ReadFile(chunkPath)
	if err != nil {
		return nil, fmt.Errorf("read chunk %s failed: %v", chunkPath, err)
	}

	// 🔥 修复：直接返回 ChunkInfo
	return &proto.ChunkInfo{
		FileId:     req.FileId,
		ChunkId:    req.ChunkId,
		Data:       data,
		ChunkIndex: 0,
		TotalChunks: 0,
	}, nil
}
// Close 关闭LevelDB连接
func (s *ChunkServer) Close() error {
	return s.db.Close()
}