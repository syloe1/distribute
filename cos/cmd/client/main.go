package main

import (
	"context"
	"flag"
	"log"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"github.com/syloe1/cos/internal/proto"  // 替换syloe1
	"github.com/syloe1/cos/pkg/utils"
)

const (
	gatewayAddr = "localhost:50052"  // Gateway地址（与Gateway一致）
)

func main() {
	// 命令行参数：操作类型（upload/download）、文件路径/FileID、保存路径
	opType := flag.String("op", "", "operation type: upload / download")
	filePath := flag.String("path", "", "local file path (for upload) / save path (for download)")
	fileID := flag.String("fileid", "", "file ID (for download)")
	flag.Parse()

	// 校验参数
	if *opType == "" || *filePath == "" {
		log.Fatal("usage: client -op [upload/download] -path [file path/save path] [-fileid fileID (for download)]")
	}

	// 连接Gateway
	conn, err := grpc.Dial(gatewayAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Fatalf("connect gateway failed: %v", err)
	}
	defer conn.Close()
	client := proto.NewObjectServiceClient(conn)

	// 执行上传/下载
	switch *opType {
	case "upload":
		uploadFile(client, *filePath)
	case "download":
		if *fileID == "" {
			log.Fatal("download need -fileid parameter")
		}
		downloadFile(client, *fileID, *filePath)
	default:
		log.Fatal("op must be upload or download")
	}
}

// uploadFile 上传文件到Gateway
func uploadFile(client proto.ObjectServiceClient, filePath string) {
	// 1. 拆分文件为块
	chunks, err := utils.SplitFile(filePath)
	if err != nil {
		log.Fatalf("split file failed: %v", err)
	}
	if len(chunks) == 0 {
		log.Fatal("empty file")
	}
	fileID := chunks[0].FileID
	log.Printf("start upload file %s (fileID: %s, total chunks: %d)", filePath, fileID, len(chunks))

	// 2. 流式上传到Gateway
	stream, err := client.UploadFile(context.Background())
	if err != nil {
		log.Fatalf("create upload stream failed: %v", err)
	}

	// 发送所有块
	for _, chunk := range chunks {
		// 转换为Proto的ChunkInfo
		protoChunk := &proto.ChunkInfo{
			FileId:     chunk.FileID,
			ChunkId:    chunk.ChunkID,
			Data:       chunk.Data,
			ChunkIndex: int32(chunk.ChunkIndex),
			TotalChunks: int32(chunk.TotalChunks),
		}

		if err := stream.Send(protoChunk); err != nil {
			log.Fatalf("send chunk %s failed: %v", chunk.ChunkID, err)
		}
		log.Printf("sent chunk %d/%d", chunk.ChunkIndex+1, chunk.TotalChunks)
	}

	// 接收上传结果
	resp, err := stream.CloseAndRecv()
	if err != nil {
		log.Fatalf("upload failed: %v", err)
	}
	log.Printf("upload success! FileID: %s, Message: %s", resp.FileId, resp.Message)
}

// downloadFile 从Gateway下载文件
func downloadFile(client proto.ObjectServiceClient, fileID, savePath string) {
	log.Printf("start download file (fileID: %s) to %s", fileID, savePath)

	// 1. 向Gateway请求下载
	stream, err := client.DownloadFile(context.Background(), &proto.DownloadFileRequest{
		FileId:   fileID,
		SavePath: savePath,  // Gateway暂不用，Client自己用
	})
	if err != nil {
		log.Fatalf("create download stream failed: %v", err)
	}

	// 2. 接收分块并合并
	var chunks []*utils.ChunkData
	for {
		protoChunk, err := stream.Recv()
		if err != nil {
			// 接收完毕，合并文件
			break
		}

		// 转换为工具类的ChunkData
		chunk := &utils.ChunkData{
			FileID:     protoChunk.FileId,
			ChunkID:    protoChunk.ChunkId,
			Data:       protoChunk.Data,
			ChunkIndex: int(protoChunk.ChunkIndex),
			TotalChunks: int(protoChunk.TotalChunks),
		}
		chunks = append(chunks, chunk)
		log.Printf("received chunk %s", protoChunk.ChunkId)
	}

	// 3. 合并块为文件
	if err := utils.MergeChunks(savePath, chunks); err != nil {
		log.Fatalf("merge chunks failed: %v", err)
	}
	log.Printf("download success! File saved to %s", savePath)
}