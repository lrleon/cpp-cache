cmake_minimum_required(VERSION 3.22)
project(cpp_cache)

set(CMAKE_CXX_STANDARD 20)


set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3 -DDEBUG=1 -pthread")

include_directories(./Aleph-w)
link_directories(./Aleph-w)

add_executable(cpp_cache_test cpp-cache_test.cpp)
target_include_directories(cpp_cache_test PRIVATE "~/cereal/include")
target_link_libraries(cpp_cache_test
    PRIVATE
    Aleph
    gtest_main
    gtest
    pthread
    lz4
)
