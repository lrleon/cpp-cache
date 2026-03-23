cmake_minimum_required(VERSION 3.22)
project(cpp_cache VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------
# Sanitizer targets (opt-in via -DSANITIZE=address|thread|undefined)
# ----------------------------------------------------------------
if(SANITIZE)
  set(SAN_FLAGS "-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SAN_FLAGS}")
endif()

# Aleph-w
set(ALEPH_DIR "$ENV{HOME}/Aleph-w" CACHE PATH "Path to Aleph-w")

# Al tener su propio CMakeLists.txt, podemos incluirlo como un subproyecto.
# Esto hace que CMake se encargue de compilar Aleph automáticamente antes 
# de enlazarlo con tus tests, resolviendo el problema en CI.
add_subdirectory(${ALEPH_DIR} ${CMAKE_BINARY_DIR}/aleph-build)

# Interface target para incluir los headers de Aleph sin contaminar el scope global
add_library(aleph_w INTERFACE)
target_include_directories(aleph_w INTERFACE ${ALEPH_DIR})

# Public headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# ----------------------------------------------------------------
# v2 unit tests
# ----------------------------------------------------------------
add_executable(cache_unit_test tests/unit/cache_test.cpp)
target_link_libraries(cache_unit_test PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# v2 concurrency tests
# ----------------------------------------------------------------
add_executable(cache_concurrency_test tests/concurrency/concurrency_test.cpp)
target_link_libraries(cache_concurrency_test PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# v2 hardened unit tests (edge cases, bug-finding)
# ----------------------------------------------------------------
add_executable(cache_hardened_test tests/unit/cache_hardened_test.cpp)
target_link_libraries(cache_hardened_test PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# v2 hardened concurrency tests (races, deadlocks, saturation)
# ----------------------------------------------------------------
add_executable(cache_concurrency_hardened_test tests/concurrency/concurrency_hardened_test.cpp)
target_link_libraries(cache_concurrency_hardened_test PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)

# ----------------------------------------------------------------
# Testing
# ----------------------------------------------------------------
include(CTest)
enable_testing()

add_test(NAME cache_unit_test COMMAND cache_unit_test)
add_test(NAME cache_concurrency_test COMMAND cache_concurrency_test)
add_test(NAME cache_hardened_test COMMAND cache_hardened_test)
add_test(NAME cache_concurrency_hardened_test COMMAND cache_concurrency_hardened_test)
