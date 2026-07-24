#pragma once

#include <muduo/base/Logging.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/TcpConnection.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dvk {

// ============================================================
// Node — 集群中的一个物理节点
// ============================================================
struct Node {
  // name + ip + port
  std::string id;    // e.g. "node-1"
  std::string host;  // e.g. "127.0.0.1"
                     // uint16_t 2个字节
  uint16_t port = 0; // e.g. 7001

  std::string address() const { return host + ":" + std::to_string(port); }
};

inline bool operator==(const Node &a, const Node &b) {
  return a.id == b.id && a.host == b.host && a.port == b.port;
}

// ============================================================
// Command — 解析后的命令
// ============================================================
// 命令枚举
enum class Command { SET, GET, DEL, PING, UNKNOWN };
// 解析结果载体
/*
- SET：key + value
- GET/DEL：仅 key
- PING：无 key/value

*/
struct ParsedCommand {
  Command type = Command::UNKNOWN;
  std::string key;
  std::string value;
};

// ============================================================
// RESP 协议常量 (Redis Serialization Protocol 风格)
// ============================================================
constexpr const char *RESP_OK = "+OK\r\n";
constexpr const char *RESP_NIL = "$-1\r\n";
constexpr const char *RESP_PONG = "+PONG\r\n";
constexpr const char *RESP_TRUE = ":1\r\n";
constexpr const char *RESP_FALSE = ":0\r\n";
inline std::string makeBulkString(const std::string &s) {
  return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

inline std::string makeError(const std::string &msg) {
  return "-ERR " + msg + "\r\n";
}

// ============================================================
// Protocol — 行协议解析器
//
// 请求格式:  COMMAND <arg1> <arg2>\r\n
// 支持命令:  SET <key> <value>     GET <key>     DEL <key>     PING
//
// 实现要点:
//   1. 用 muduo Buffer::findCRLF() 找 \r\n
//   2. 不完整行 → 返回 false, Buffer 保留数据等下次回调
//   3. SET 命令中 value 可含空格 (将 tokens[2..] 用空格 join)
// ============================================================
/*
\r\n作为请求分隔符， 处理TCP半包， 缓冲区数据不足一行时不消费， 直接返回

*/
class Protocol {
public:
  // 从 Buffer 中解析一条完整命令。成功返回 true 并消费数据。
  // Buffer 中数据不完整时返回 false,不做任何消费。
  static bool parseCommand(muduo::net::Buffer *buf, ParsedCommand *cmd) {
    // 1. 在缓冲区查找 \\r\\n 分隔符
    const char *crlf = buf->findCRLF();
    if (!crlf) {
      return false; // 半包，等更多数据
    }

    // 取出整行（不含 \r\n）
    std::string line(buf->peek(), crlf - buf->peek());
    // 缓冲区消费：当前行 + \\r\\n 两个字节
    buf->retrieveUntil(crlf + 2); // 消费 line + \r\n
                                  // 空行直接标记未知命令
    if (line.empty()) {
      cmd->type = Command::UNKNOWN;
      return true;
    }

    // 按空格 tokenize
    std::vector<std::string> tokens = split(line, ' ');
    if (tokens.empty()) {
      cmd->type = Command::UNKNOWN;
      return true;
    }

    // 命令名转大写
    std::string cmdName = toUpper(tokens[0]);
    // set k v  至少3个token
    if (cmdName == "SET" && tokens.size() >= 3) {
      cmd->type = Command::SET;
      cmd->key = tokens[1];
      // value = tokens[2..] 用空格 join，支持含空格的值
      cmd->value =
          join(std::vector<std::string>(tokens.begin() + 2, tokens.end()), " ");
    } else if (cmdName == "GET" && tokens.size() >= 2) {
      cmd->type = Command::GET;
      cmd->key = tokens[1];
    } else if (cmdName == "DEL" && tokens.size() >= 2) {
      cmd->type = Command::DEL;
      cmd->key = tokens[1];
    } else if (cmdName == "PING") {
      cmd->type = Command::PING;
    } else {
      cmd->type = Command::UNKNOWN;
    }

    return true;
  }

  // 序列化一条 SET/DEL 命令（用于复制）
  static std::string serializeSET(const std::string &key,
                                  const std::string &value) {
    return "SET " + key + " " + value + "\r\n";
  }
  static std::string serializeDEL(const std::string &key) {
    return "DEL " + key + "\r\n";
  }

private:
  // command -> tokens
  static std::vector<std::string> split(const std::string &s, char delim) {
    // 存储切割后的结果
    std::vector<std::string> result;
    // 把字符串 s 包装成字符串流，方便 getline 读取分段
    std::istringstream iss(s);
    std::string token;

    // 循环按分隔符读取一段，存入 token
    while (std::getline(iss, token, delim)) {
      // 关键：空字符串不加入结果，自动忽略连续多个分隔符
      if (!token.empty()) {
        result.emplace_back(std::move(token));
      }
    }
    return result;
  }

  static std::string join(const std::vector<std::string> &parts,
                          const std::string &delim) {
    // 如果数组为空，直接返回空字符串
    if (parts.empty())
      return "";
    // 先把第一个元素拿出来作为初始结果
    std::string result = parts[0];
    // 从第二个元素开始循环，每次：分隔符 + 当前片段 追加到结果末尾
    for (size_t i = 1; i < parts.size(); ++i) {
      result += delim + parts[i];
    }
    return result;
  }
  // 值传递
  //::toupper(c)C标准库函数
  static std::string toUpper(std::string s) {
    for (auto &c : s)
      c = static_cast<char>(::toupper(c));
    return s;
  }
};

} // namespace dvk
