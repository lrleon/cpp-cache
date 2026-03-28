cmake_minimum_required(VERSION 3.22)
project(cpp_cache VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------
# Sanitizer targets (opt-in via -DSANITIZE=address|thread|undefined)
# ----------------------------------------------------------------
if(SANITIZE)
  set(SAN_FLAGS "-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g")
  # Aleph-w's LINKNAME_TO_TYPE macro uses manual offsetof via null pointer
  # arithmetic (dlink.H), which triggers UBSan's null check. Suppress it.
  if(SANITIZE MATCHES "undefined")
    set(SAN_FLAGS "${SAN_FLAGS} -fno-sanitize=null")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SAN_FLAGS}")
endif()

# Aleph-w
if(NOT ALEPH_DIR AND DEFINED ENV{ALEPH_DIR} AND NOT "$ENV{ALEPH_DIR}" STREQUAL "")
  set(ALEPH_DIR "$ENV{ALEPH_DIR}")
endif()

if(NOT ALEPH_DIR)
  foreach(candidate
      "${CMAKE_SOURCE_DIR}/../Aleph-w"
      "${CMAKE_SOURCE_DIR}/../../Aleph-w"
      "$ENV{HOME}/Aleph-w")
    if(EXISTS "${candidate}/CMakeLists.txt")
      set(ALEPH_DIR "${candidate}")
      break()
    endif()
  endforeach()
endif()

set(ALEPH_DIR "${ALEPH_DIR}" CACHE PATH "Path to Aleph-w")

if(NOT ALEPH_DIR OR NOT EXISTS "${ALEPH_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "ALEPH_DIR must point to an Aleph-w checkout containing CMakeLists.txt")
endif()

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
# Test helpers
# ----------------------------------------------------------------
function(add_cache_test target_name source_file)
  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name} PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# ----------------------------------------------------------------
# v2 tests
# ----------------------------------------------------------------
add_cache_test(cache_unit_test tests/unit/cache_test.cpp)
add_cache_test(cache_concurrency_test tests/concurrency/concurrency_test.cpp)
add_cache_test(cache_hardened_test tests/unit/cache_hardened_test.cpp)
add_cache_test(cache_concurrency_hardened_test tests/concurrency/concurrency_hardened_test.cpp)

# ----------------------------------------------------------------
# Testing
# ----------------------------------------------------------------
include(CTest)
enable_testing()

set_tests_properties(
    cache_unit_test 
    cache_concurrency_test 
    cache_hardened_test 
    cache_concurrency_hardened_test 
    PROPERTIES LABELS cpp_cache
)

# ----------------------------------------------------------------
# Valgrind concurrency test (NOT in CTest — run manually)
#
# Build:  cmake --build build-v2 --target cache_valgrind_test
# Run:    scripts/run_valgrind.rb  (or directly with valgrind)
# ----------------------------------------------------------------
add_executable(cache_valgrind_test EXCLUDE_FROM_ALL tests/valgrind/cache_valgrind_test.cpp)
target_link_libraries(cache_valgrind_test PRIVATE aleph_w Aleph gtest_main gtest pthread gsl gslcblas)
