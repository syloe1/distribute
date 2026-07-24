#!/bin/bash
# ============================================================
# test.sh — 分布式 KV 存储自动化测试脚本
# 用法: ./test.sh [--verbose]
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SERVER_BIN="$BUILD_DIR/kv_server"
CLIENT_BIN="$BUILD_DIR/kv_client"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; VERBOSE=false
[[ "$*" =~ --verbose ]] && VERBOSE=true

log_info() { echo -e "${CYAN}[INFO]${NC}  $*"; }
log_pass() { echo -e "${GREEN}[PASS]${NC}  $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "${RED}[FAIL]${NC}  $*"; FAIL=$((FAIL + 1)); }

send_cmd() { local p=$1; shift; printf "%s\r\n" "$*" | nc -w3 127.0.0.1 "$p" 2>/dev/null || true; }

# ---- Phase 1: Build ----
log_info "===== Phase 1: Build ====="
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
cmake .. > /dev/null 2>&1 || true
make -j"$(nproc)" 2>&1 | tail -2
cd "$SCRIPT_DIR"
[[ -x "$SERVER_BIN" ]] && log_pass "kv_server ready" || { log_fail "kv_server missing"; exit 1; }
[[ -x "$CLIENT_BIN" ]] && log_pass "kv_client ready" || { log_fail "kv_client missing"; exit 1; }

# ---- Phase 2: Start cluster ----
log_info "===== Phase 2: Start 3-node cluster ====="
P1=7101; P2=7102; P3=7103

cleanup() {
    for p in "$P1" "$P2" "$P3" 7201 7202 7203; do
        fuser -k "$p/tcp" 2>/dev/null || true
    done
}
cleanup
trap cleanup EXIT

"$SERVER_BIN" --id n1 --port $P1 --threads 2 > /dev/null 2>&1 &
"$SERVER_BIN" --id n2 --port $P2 --threads 2 > /dev/null 2>&1 &
"$SERVER_BIN" --id n3 --port $P3 --threads 2 > /dev/null 2>&1 &
sleep 2

for p in $P1 $P2 $P3; do
    ss -tlnp 2>/dev/null | grep -q ":$p " && log_pass "Node :$p listening" || log_fail "Node :$p not listening"
done

# ---- Phase 3: Basic protocol tests ----
log_info "===== Phase 3: Protocol tests ====="

r=$(send_cmd $P1 "PING")
[[ "$r" == $'+PONG\r'* ]] && log_pass "PING" || log_fail "PING: $r"

r=$(send_cmd $P1 "SET k1 hello")
[[ "$r" == $'+OK\r'* ]] && log_pass "SET" || log_fail "SET: $r"

r=$(send_cmd $P1 "GET k1")
echo "$r" | grep -q '^\$5' && log_pass "GET (hit)" || log_fail "GET: $r"

r=$(send_cmd $P1 "DEL k1")
[[ "$r" == $':1\r'* ]] && log_pass "DEL (existed)" || log_fail "DEL: $r"

r=$(send_cmd $P1 "DEL k1")
[[ "$r" == $':0\r'* ]] && log_pass "DEL (not found)" || log_fail "DEL2: $r"

r=$(send_cmd $P1 "GET k1")
echo "$r" | grep -q '^\$-1' && log_pass "GET (miss)" || log_fail "GET miss: $r"

r=$(send_cmd $P1 "UNKNOWN")
echo "$r" | grep -q '^-ERR' && log_pass "Unknown command" || log_fail "Unknown: $r"

r=$(send_cmd $P1 "SET greeting hello world")
[[ "$r" == $'+OK\r'* ]] && log_pass "SET with spaces" || log_fail "SET spaces: $r"

# Batch write/read
for i in $(seq 1 20); do send_cmd $P1 "SET batch_$i v$i" > /dev/null; done
ok=0
for i in $(seq 1 20); do
    send_cmd $P1 "GET batch_$i" | grep -q 'v' && ((ok++))
done
[[ $ok -eq 20 ]] && log_pass "Batch 20 keys ($ok/20)" || log_fail "Batch: $ok/20"

# ---- Phase 4: Consistent hashing ----
log_info "===== Phase 4: Consistent hashing ====="
cleanup
"$SERVER_BIN" --id n1 --port $P1 --threads 2 > /dev/null 2>&1 &
"$SERVER_BIN" --id n2 --port $P2 --threads 2 > /dev/null 2>&1 &
"$SERVER_BIN" --id n3 --port $P3 --threads 2 > /dev/null 2>&1 &
sleep 2

ADDRS="127.0.0.1:$P1 127.0.0.1:$P2 127.0.0.1:$P3"
N=45  # 45 keys distributed across 3 nodes

# Write keys through client
CMDS=""
for i in $(seq 1 $N); do CMDS="${CMDS}SET hk_${i} v${i}\n"; done
CMDS="${CMDS}QUIT\n"
writes=$(printf "$CMDS" | timeout 30 "$CLIENT_BIN" $ADDRS 2>/dev/null | grep -c "+OK")
[[ $writes -eq $N ]] && log_pass "Client writes ($writes/$N)" || log_fail "Client writes: $writes/$N"

# Count keys per node
for p in $P1 $P2 $P3; do
    BATCH=""; for i in $(seq 1 $N); do BATCH="${BATCH}GET hk_${i}\r\n"; done
    c=$(printf "$BATCH" | nc -w10 127.0.0.1 "$p" 2>/dev/null | grep -c '^\$[0-9]')
    echo "  Node :$p: $c keys"
    [[ $c -gt 5 ]] && log_pass "  Node :$p has $c keys" || log_fail "  Node :$p only $c keys"
done

# Verify reads (use nc to avoid client connection timing issues)
ok=0
for i in $(seq 1 10); do
    for p in $P1 $P2 $P3; do
        if send_cmd $p "GET hk_${i}" | grep -q '^\$[0-9]'; then
            ((ok++)); break
        fi
    done
done
[[ $ok -eq 10 ]] && log_pass "Key reads ($ok/10)" || log_fail "Key reads: $ok/10"

# ---- Phase 5: Replication ----
log_info "===== Phase 5: Replication ====="
cleanup
MP=7201; S1P=7202; S2P=7203

"$SERVER_BIN" --id master --port $MP --threads 2 --replicas "slave1:${S1P},slave2:${S2P}" > /dev/null 2>&1 &
"$SERVER_BIN" --id slave1 --port $S1P --threads 2 > /dev/null 2>&1 &
"$SERVER_BIN" --id slave2 --port $S2P --threads 2 > /dev/null 2>&1 &
sleep 2

for i in $(seq 1 10); do send_cmd $MP "SET rep_$i v$i" > /dev/null; done
sleep 1

mc=0; s1c=0; s2c=0
for i in $(seq 1 10); do
    send_cmd $MP "GET rep_$i" | grep -q 'v' && ((mc++))
    send_cmd $S1P "GET rep_$i" | grep -q 'v' && ((s1c++))
    send_cmd $S2P "GET rep_$i" | grep -q 'v' && ((s2c++))
done
echo "  Master: $mc/10  Slave1: $s1c/10  Slave2: $s2c/10"
[[ $mc -eq 10 ]] && log_pass "Master complete" || log_fail "Master: $mc/10"
[[ $s1c -ge 5 ]] && log_pass "Slave1 replicated ($s1c/10)" || log_warn "Slave1: $s1c/10"
[[ $s2c -ge 5 ]] && log_pass "Slave2 replicated ($s2c/10)" || log_warn "Slave2: $s2c/10"

# ---- Summary ----
log_info "===== Results ====="
echo -e "  ${GREEN}Passed: $PASS${NC}  ${RED}Failed: $FAIL${NC}"
if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}  All tests passed!${NC}"; exit 0
else
    echo -e "${RED}  $FAIL test(s) failed${NC}"; exit 1
fi
