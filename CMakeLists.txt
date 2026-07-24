cmake_minimum_required(VERSION 3.16)
project(distributed-kv VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -O2")

# muduo installed to local prefix
set(MUDUO_PREFIX /home/wk/.local)
include_directories(${MUDUO_PREFIX}/include)
link_directories(${MUDUO_PREFIX}/lib)

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ---- Server ----
add_executable(kv_server src/kv_server.cpp)
target_link_libraries(kv_server
    muduo_net muduo_base
    Threads::Threads pthread
    OpenSSL::SSL OpenSSL::Crypto
)

# ---- Client ----
add_executable(kv_client src/kv_client.cpp)
target_link_libraries(kv_client
    muduo_net muduo_base
    Threads::Threads pthread
    OpenSSL::SSL OpenSSL::Crypto
)

# ---- Tests ----
enable_testing()
add_subdirectory(tests)
