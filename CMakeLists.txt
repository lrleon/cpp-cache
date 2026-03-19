cmake_minimum_required(VERSION 3.22)
project(cpp_cache VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Aleph-w
set(ALEPH_DIR "$ENV{HOME}/Aleph-w" CACHE PATH "Path to Aleph-w")
include_directories(${ALEPH_DIR})
link_directories(${ALEPH_DIR})

# Public headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# ----------------------------------------------------------------
# Legacy v1 test (kept for reference)
# ----------------------------------------------------------------
add_executable(cpp_cache_test_v1 cpp-cache_test.cpp)
target_include_directories(cpp_cache_test_v1 PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(cpp_cache_test_v1 PRIVATE Aleph gtest_main gtest pthread lz4)

# ----------------------------------------------------------------
# v2 unit tests
# ----------------------------------------------------------------
add_executable(cache_unit_test tests/unit/cache_test.cpp ${ALEPH_DIR}/hash-fct.C)
target_link_libraries(cache_unit_test PRIVATE Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# v2 concurrency tests
# ----------------------------------------------------------------
add_executable(cache_concurrency_test tests/concurrency/concurrency_test.cpp ${ALEPH_DIR}/hash-fct.C)
target_link_libraries(cache_concurrency_test PRIVATE Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# Sanitizer targets (opt-in via -DSANITIZE=address|thread|undefined)
# ----------------------------------------------------------------
if(SANITIZE)
  set(SAN_FLAGS "-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SAN_FLAGS}")
endif()
