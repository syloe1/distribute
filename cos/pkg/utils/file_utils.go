package utils

import (
	"crypto/rand"
	"encoding/hex"
	"os"
	"fmt"
	"path/filepath"
)

const (
	ChunkSize = 16 * 1024 * 1024  // 单块16MB（可自定义）
)

// GenerateFileID 生成唯一文件ID（UUID简化版）
func GenerateFileID() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// SplitFile 拆分文件为块（返回块列表）
func SplitFile(filePath string) ([]*ChunkData, error) {
	// 打开文件
	file, err := os.Open(filePath)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	// 获取文件信息
	fileInfo, err := file.Stat()
	if err != nil {
		return nil, err
	}
	fileSize := fileInfo.Size()

	// 计算总块数
	totalChunks := int((fileSize + ChunkSize - 1) / ChunkSize)
	fileID := GenerateFileID()
	chunks := make([]*ChunkData, 0, totalChunks)

	// 分块读取
	buf := make([]byte, ChunkSize)
	for i := 0; i < totalChunks; i++ {
		n, err := file.Read(buf)
		if err != nil && err.Error() != "EOF" {
			return nil, err
		}

		// 生成块ID（fileID + 块序号）
		chunkID := fmt.Sprintf("%s_chunk%d", fileID, i)
		chunks = append(chunks, &ChunkData{
			FileID:     fileID,
			ChunkID:    chunkID,
			Data:       buf[:n],
			ChunkIndex: i,
			TotalChunks: totalChunks,
		})
	}

	return chunks, nil
}

// ChunkData 块数据结构体（与Proto的ChunkInfo对应）
type ChunkData struct {
	FileID     string
	ChunkID    string
	Data       []byte
	ChunkIndex int
	TotalChunks int
}

// MergeChunks 合并块为文件
func MergeChunks(savePath string, chunks []*ChunkData) error {
	// 创建保存目录
	saveDir := filepath.Dir(savePath)
	if err := os.MkdirAll(saveDir, 0755); err != nil {
		return err
	}

	// 创建文件并写入块数据（按序号排序，这里假设接收顺序正确）
	file, err := os.Create(savePath)
	if err != nil {
		return err
	}
	defer file.Close()

	for _, chunk := range chunks {
		if _, err := file.Write(chunk.Data); err != nil {
			return err
		}
	}

	return nil
}