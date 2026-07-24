# ============================================================
# tests/CMakeLists.txt — 单元测试 & 压测
# ============================================================

# 公用的 include 路径 (指向 src/)
set(TEST_COMMON_INCLUDES ${CMAKE_SOURCE_DIR}/src)

# ---- KVStore 单元测试 ----
add_executable(kv_store_test kv_store_test.cc)
target_include_directories(kv_store_test PRIVATE ${TEST_COMMON_INCLUDES})
target_link_libraries(kv_store_test
    muduo_base
    Threads::Threads pthread
)
add_test(NAME kv_store_test COMMAND kv_store_test)

# ---- HashRing 单元测试 ----
add_executable(hash_ring_test hash_ring_test.cc)
target_include_directories(hash_ring_test PRIVATE ${TEST_COMMON_INCLUDES})
target_link_libraries(hash_ring_test
    muduo_base
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads pthread
)
add_test(NAME hash_ring_test COMMAND hash_ring_test)

# ---- Protocol 单元测试 ----
add_executable(protocol_test protocol_test.cc)
target_include_directories(protocol_test PRIVATE ${TEST_COMMON_INCLUDES})
target_link_libraries(protocol_test
    muduo_net muduo_base
    Threads::Threads pthread
)
add_test(NAME protocol_test COMMAND protocol_test)

# ---- 压测 ----
# 需要启动服务端，默认不加入 ctest
add_executable(bench bench.cc)
target_include_directories(bench PRIVATE ${TEST_COMMON_INCLUDES})
target_link_libraries(bench
    muduo_net muduo_base
    Threads::Threads pthread
)
