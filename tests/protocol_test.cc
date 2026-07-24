// ============================================================
// protocol_test.cc — Protocol (RESP 行协议) 单元测试
//
// 测试范围:
//   1. 基本命令解析 (SET/GET/DEL/PING)
//   2. 边界情况 (空行、未知命令、大小写)
//   3. 多空格 value (SET key hello world)
//   4. TCP 半包处理 (不完整行不消费)
//   5. 多命令粘包 (一次 Buffer 含多条命令)
//   6. 序列化 (serializeSET / serializeDEL)
// ============================================================

#include "common.h"

#include <muduo/net/Buffer.h>

#include <cstring>
#include <iostream>
#include <string>

using namespace dvk;
using namespace muduo::net;

// ---- 简易测试框架 ----
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)                                                            \
  static void test_##name();                                                  \
  struct Registrar_##name {                                                   \
    Registrar_##name() { registerTest(#name, test_##name); }                  \
  } registrar_##name;                                                         \
  static void test_##name()

using TestFunc = void (*)();
struct TestEntry {
  const char *name;
  TestFunc func;
};
static std::vector<TestEntry> &allTests() {
  static std::vector<TestEntry> tests;
  return tests;
}
void registerTest(const char *name, TestFunc func) {
  allTests().push_back({name, func});
}

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "  FAILED: " << __FILE__ << ":" << __LINE__ << " ["       \
                << #cond << "]\n";                                            \
      g_failed++;                                                             \
      return;                                                                 \
    } else {                                                                  \
      g_passed++;                                                             \
    }                                                                         \
  } while (0)

// ---- 辅助：往 Buffer 里写入字符串 ----
static void appendToBuffer(Buffer *buf, const std::string &data) {
  buf->append(data.data(), data.size());
}

// ============================================================
// 1. 基本命令解析
// ============================================================

TEST(parse_ping) {
  Buffer buf;
  appendToBuffer(&buf, "PING\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::PING);
  EXPECT(buf.readableBytes() == 0); // 数据已消费
}

TEST(parse_set) {
  Buffer buf;
  appendToBuffer(&buf, "SET mykey myvalue\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::SET);
  EXPECT(cmd.key == "mykey");
  EXPECT(cmd.value == "myvalue");
  EXPECT(buf.readableBytes() == 0);
}

TEST(parse_get) {
  Buffer buf;
  appendToBuffer(&buf, "GET k1\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::GET);
  EXPECT(cmd.key == "k1");
}

TEST(parse_del) {
  Buffer buf;
  appendToBuffer(&buf, "DEL k1\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::DEL);
  EXPECT(cmd.key == "k1");
}

TEST(parse_unknown) {
  Buffer buf;
  appendToBuffer(&buf, "FOOBAR\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::UNKNOWN);
  EXPECT(buf.readableBytes() == 0);
}

TEST(parse_empty_line) {
  Buffer buf;
  appendToBuffer(&buf, "\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::UNKNOWN);
}

// ============================================================
// 2. 多空格 value
// ============================================================

TEST(parse_set_multispace_value) {
  Buffer buf;
  appendToBuffer(&buf, "SET greeting hello beautiful world\r\n");

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);
  EXPECT(ok);
  EXPECT(cmd.type == Command::SET);
  EXPECT(cmd.key == "greeting");
  EXPECT(cmd.value == "hello beautiful world");
}

TEST(parse_set_value_with_leading_trailing_spaces) {
  // Note: 当前 split 实现会跳过空 token，
  // 所以连续空格被折叠。这是已知行为。
  Buffer buf;
  appendToBuffer(&buf, "SET k a  b\r\n"); // 双空格

  ParsedCommand cmd;
  Protocol::parseCommand(&buf, &cmd);
  // value = "a b" (中间双空格被 join 合并为单空格)
  EXPECT(cmd.value == "a b");
}

// ============================================================
// 3. 大小写不敏感
// ============================================================

TEST(case_insensitive_commands) {
  // 小写
  {
    Buffer buf;
    appendToBuffer(&buf, "set k v\r\n");
    ParsedCommand cmd;
    Protocol::parseCommand(&buf, &cmd);
    EXPECT(cmd.type == Command::SET);
    EXPECT(cmd.key == "k");
    EXPECT(cmd.value == "v");
  }
  // 混合大小写
  {
    Buffer buf;
    appendToBuffer(&buf, "Get somekey\r\n");
    ParsedCommand cmd;
    Protocol::parseCommand(&buf, &cmd);
    EXPECT(cmd.type == Command::GET);
    EXPECT(cmd.key == "somekey");
  }
  // 全大写
  {
    Buffer buf;
    appendToBuffer(&buf, "DEL x\r\n");
    ParsedCommand cmd;
    Protocol::parseCommand(&buf, &cmd);
    EXPECT(cmd.type == Command::DEL);
  }
  // ping 各种写法
  {
    Buffer buf;
    appendToBuffer(&buf, "ping\r\n");
    ParsedCommand cmd;
    Protocol::parseCommand(&buf, &cmd);
    EXPECT(cmd.type == Command::PING);
  }
}

// ============================================================
// 4. TCP 半包 — 不完整行不消费
// ============================================================

TEST(partial_packet_no_crlf) {
  Buffer buf;
  appendToBuffer(&buf, "SET k v"); // 没有 \r\n

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);

  EXPECT(!ok);                          // 返回 false 表示数据不完整
  EXPECT(buf.readableBytes() == 7);     // "SET k v" 原封不动保留
}

TEST(partial_packet_half_crlf) {
  Buffer buf;
  appendToBuffer(&buf, "GET x\r"); // 只有 \r 没有 \n

  ParsedCommand cmd;
  bool ok = Protocol::parseCommand(&buf, &cmd);

  EXPECT(!ok); // 不完整
  EXPECT(buf.readableBytes() == 6);
}

// ============================================================
// 5. 半包后补全数据 — 模拟 TCP 流到达
// ============================================================

TEST(partial_then_complete) {
  Buffer buf;

  // 第一次到达: "SET"
  appendToBuffer(&buf, "SET");
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(!ok); // 不完整
  }

  // 第二次到达: " k v\r\n"
  appendToBuffer(&buf, " k v\r\n");
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok); // 现在完整了
    EXPECT(cmd.type == Command::SET);
    EXPECT(cmd.key == "k");
    EXPECT(cmd.value == "v");
  }

  EXPECT(buf.readableBytes() == 0);
}

// ============================================================
// 6. 多命令粘包
// ============================================================

TEST(multiple_commands_in_one_buffer) {
  Buffer buf;
  appendToBuffer(&buf, "PING\r\nSET a 1\r\nGET a\r\n");

  // 第 1 条
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok);
    EXPECT(cmd.type == Command::PING);
  }

  // 第 2 条
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok);
    EXPECT(cmd.type == Command::SET);
    EXPECT(cmd.key == "a");
    EXPECT(cmd.value == "1");
  }

  // 第 3 条
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok);
    EXPECT(cmd.type == Command::GET);
    EXPECT(cmd.key == "a");
  }

  EXPECT(buf.readableBytes() == 0);
}

TEST(multiple_commands_with_partial_last) {
  Buffer buf;
  // 2 条完整 + 1 条不完整
  appendToBuffer(&buf, "PING\r\nDEL x\r\nSET incompl");

  // 第 1 条
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok);
    EXPECT(cmd.type == Command::PING);
  }

  // 第 2 条
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(ok);
    EXPECT(cmd.type == Command::DEL);
    EXPECT(cmd.key == "x");
  }

  // 第 3 条 — 不完整
  {
    ParsedCommand cmd;
    bool ok = Protocol::parseCommand(&buf, &cmd);
    EXPECT(!ok);
    EXPECT(buf.readableBytes() == 11); // "SET incompl" 保留
  }
}

// ============================================================
// 7. 序列化
// ============================================================

TEST(serialize_set) {
  std::string result = Protocol::serializeSET("hello", "world");
  EXPECT(result == "SET hello world\r\n");
}

TEST(serialize_set_multispace_value) {
  std::string result = Protocol::serializeSET("k", "hello world");
  EXPECT(result == "SET k hello world\r\n");
}

TEST(serialize_del) {
  std::string result = Protocol::serializeDEL("mykey");
  EXPECT(result == "DEL mykey\r\n");
}

// ============================================================
// 8. key 含特殊字符
// ============================================================

TEST(key_with_colon) {
  Buffer buf;
  appendToBuffer(&buf, "GET user:123:email\r\n");

  ParsedCommand cmd;
  Protocol::parseCommand(&buf, &cmd);
  EXPECT(cmd.type == Command::GET);
  EXPECT(cmd.key == "user:123:email");
}

TEST(key_with_numbers) {
  Buffer buf;
  appendToBuffer(&buf, "SET 123key v\r\n");

  ParsedCommand cmd;
  Protocol::parseCommand(&buf, &cmd);
  EXPECT(cmd.type == Command::SET);
  EXPECT(cmd.key == "123key");
}

// ============================================================
// 9. 只有命令名 (缺参数) — 应为 UNKNOWN
// ============================================================

TEST(get_without_key) {
  // "GET" 后没有 key: tokens.size() >= 2 不满足
  Buffer buf;
  appendToBuffer(&buf, "GET\r\n");

  ParsedCommand cmd;
  Protocol::parseCommand(&buf, &cmd);
  EXPECT(cmd.type == Command::UNKNOWN);
}

TEST(set_without_value) {
  // "SET k" 缺少 value
  Buffer buf;
  appendToBuffer(&buf, "SET keyonly\r\n");

  ParsedCommand cmd;
  Protocol::parseCommand(&buf, &cmd);
  EXPECT(cmd.type == Command::UNKNOWN);
}

// ============================================================
// 10. RESP 辅助函数
// ============================================================

TEST(resp_bulk_string) {
  std::string s = makeBulkString("hello");
  EXPECT(s == "$5\r\nhello\r\n");
}

TEST(resp_bulk_string_empty) {
  std::string s = makeBulkString("");
  EXPECT(s == "$0\r\n\r\n");
}

TEST(resp_error) {
  std::string e = makeError("something wrong");
  EXPECT(e == "-ERR something wrong\r\n");
}

// ============================================================
// main
// ============================================================

int main() {
  std::cout << "=== Protocol Unit Tests ===\n";

  for (const auto &entry : allTests()) {
    std::cout << "  " << entry.name << " ...";
    int before = g_passed + g_failed;
    entry.func();
    int after = g_passed + g_failed;
    if (after == before) {
      std::cout << " PASS\n";
    }
  }

  std::cout << "\nResults: " << g_passed << " passed, " << g_failed
            << " failed\n";
  return g_failed > 0 ? 1 : 0;
}
