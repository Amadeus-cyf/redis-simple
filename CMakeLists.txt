cmake_minimum_required(VERSION 3.21)
project(redis_simple LANGUAGES C CXX)

option(REDIS_SIMPLE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(REDIS_SIMPLE_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)
option(REDIS_SIMPLE_USE_MIMALLOC
  "Allocate the server's C++ objects with the bundled mimalloc" ON)
option(REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC
  "Build redis_simple_benchmarks with mimalloc, to compare allocators" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(spdlog QUIET)
find_package(Threads REQUIRED)

add_custom_target(
  format
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/format.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Format project C++ sources with clang-format"
  VERBATIM
)

add_custom_target(
  check-format
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/format.sh" --check
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Check project C++ source formatting"
  VERBATIM
)

function(redis_simple_collect_sources out_var)
  set(sources)
  foreach(source_dir IN LISTS ARGN)
    file(GLOB_RECURSE dir_sources CONFIGURE_DEPENDS
      "${CMAKE_SOURCE_DIR}/${source_dir}/*.cpp"
      "${CMAKE_SOURCE_DIR}/${source_dir}/*.h"
    )
    list(APPEND sources ${dir_sources})
  endforeach()
  set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

function(redis_simple_exclude_sources source_var)
  foreach(pattern IN LISTS ARGN)
    list(FILTER ${source_var} EXCLUDE REGEX "${pattern}")
  endforeach()
  set(${source_var} ${${source_var}} PARENT_SCOPE)
endfunction()

function(redis_simple_configure_target target)
  target_compile_features(${target} PRIVATE cxx_std_23)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR})
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(REDIS_SIMPLE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(REDIS_SIMPLE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(TARGET spdlog::spdlog)
    target_compile_definitions(${target} PRIVATE REDIS_SIMPLE_USE_SPDLOG)
    target_link_libraries(${target} PRIVATE spdlog::spdlog)
  endif()
endfunction()

function(redis_simple_add_executable target)
  add_executable(${target} ${ARGN})
  redis_simple_configure_target(${target})
endfunction()

function(redis_simple_add_gtest_suite suite_name)
  add_test(
    NAME redis_simple_unit_${suite_name}
    COMMAND redis_simple_tests --gtest_filter=${suite_name}.*
  )
  set_tests_properties(redis_simple_unit_${suite_name} PROPERTIES LABELS unit)
endfunction()

redis_simple_collect_sources(REDIS_SIMPLE_CORE_SOURCES
  connection
  event_loop
  memory
  server
  data_types
  tcp
  utils
)
redis_simple_exclude_sources(REDIS_SIMPLE_CORE_SOURCES
  "_test\\.cpp$"
  "/server/main\\.cpp$"
)
if(APPLE)
  redis_simple_exclude_sources(REDIS_SIMPLE_CORE_SOURCES
    "/event_loop/epoll_poller\\.(cpp|h)$"
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  redis_simple_exclude_sources(REDIS_SIMPLE_CORE_SOURCES
    "/event_loop/kqueue_poller\\.(cpp|h)$"
  )
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

redis_simple_collect_sources(REDIS_SIMPLE_CLI_SOURCES cli)
redis_simple_exclude_sources(REDIS_SIMPLE_CLI_SOURCES "_test\\.cpp$")

add_library(redis_simple_core STATIC ${REDIS_SIMPLE_CORE_SOURCES})
redis_simple_configure_target(redis_simple_core)
target_link_libraries(redis_simple_core PUBLIC Threads::Threads)

add_library(redis_simple_cli STATIC ${REDIS_SIMPLE_CLI_SOURCES})
redis_simple_configure_target(redis_simple_cli)
target_link_libraries(redis_simple_cli PRIVATE redis_simple_core)

redis_simple_add_executable(redis_simple "server/main.cpp")
target_link_libraries(redis_simple PRIVATE redis_simple_core)

# Sanitizers replace the allocator themselves, so their builds keep the
# default one.
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize" REDIS_SIMPLE_SANITIZED)
set(REDIS_SIMPLE_MIMALLOC_ALLOWED OFF)
if(REDIS_SIMPLE_SANITIZED EQUAL -1)
  set(REDIS_SIMPLE_MIMALLOC_ALLOWED ON)
endif()
if(REDIS_SIMPLE_MIMALLOC_ALLOWED AND
   (REDIS_SIMPLE_USE_MIMALLOC OR REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC))
  # Static library only. Executables replace global operator new and delete
  # instead of overriding libc malloc, which works the same way on Linux and
  # macOS.
  set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("third_party/mimalloc" EXCLUDE_FROM_ALL)
endif()
if(REDIS_SIMPLE_MIMALLOC_ALLOWED AND REDIS_SIMPLE_USE_MIMALLOC)
  target_link_libraries(redis_simple PRIVATE mimalloc-static)
  target_compile_definitions(redis_simple PRIVATE REDIS_SIMPLE_USE_MIMALLOC)
endif()

add_subdirectory("third_party/googletest")
enable_testing()

redis_simple_collect_sources(REDIS_SIMPLE_UNIT_TEST_SOURCES
  cli
  connection
  event_loop
  memory
  server
  data_types
  tcp
  utils
)
list(FILTER REDIS_SIMPLE_UNIT_TEST_SOURCES INCLUDE REGEX "_test\\.cpp$")

redis_simple_add_executable(redis_simple_tests ${REDIS_SIMPLE_UNIT_TEST_SOURCES})
target_link_libraries(
  redis_simple_tests
  PRIVATE
    redis_simple_cli
    redis_simple_core
    GTest::gtest_main
)

redis_simple_add_gtest_suite(StringUtilsTest)
redis_simple_add_gtest_suite(Crc64Test)
redis_simple_add_gtest_suite(LzfTest)
redis_simple_add_gtest_suite(RdbTest)
redis_simple_add_gtest_suite(RdbBackgroundSaveTest)
redis_simple_add_gtest_suite(DictStringTest)
redis_simple_add_gtest_suite(DictIntTest)
redis_simple_add_gtest_suite(DynamicBufferTest)
redis_simple_add_gtest_suite(DynamicBufferDeathTest)
redis_simple_add_gtest_suite(LoopTest)
redis_simple_add_gtest_suite(IntSetTest)
redis_simple_add_gtest_suite(IntSetDeathTest)
redis_simple_add_gtest_suite(ListPackTest)
redis_simple_add_gtest_suite(ListPackReaderTest)
redis_simple_add_gtest_suite(ListPackDeathTest)
redis_simple_add_gtest_suite(QuickListTest)
redis_simple_add_gtest_suite(ReplyBufferTest)
redis_simple_add_gtest_suite(SkiplistTest)
redis_simple_add_gtest_suite(SkiplistDeathTest)
redis_simple_add_gtest_suite(CustomSkiplistTest)
redis_simple_add_gtest_suite(CommandTableTest)
redis_simple_add_gtest_suite(RedisDbTest)
redis_simple_add_gtest_suite(RedisObjectTest)
redis_simple_add_gtest_suite(RedisObjectDeathTest)
redis_simple_add_gtest_suite(AsyncReclaimerTest)
redis_simple_add_gtest_suite(AofTest)
redis_simple_add_gtest_suite(ManifestTest)
redis_simple_add_gtest_suite(ServerOptionsTest)
redis_simple_add_gtest_suite(IoThreadTest)
redis_simple_add_gtest_suite(ShutdownTest)
redis_simple_add_gtest_suite(ReplyTest)
redis_simple_add_gtest_suite(ReplyDeathTest)
redis_simple_add_gtest_suite(FloatUtilsTest)
redis_simple_add_gtest_suite(IntUtilsTest)
redis_simple_add_gtest_suite(SipHashTest)
redis_simple_add_gtest_suite(SplitArgsTest)
redis_simple_add_gtest_suite(ListTest)
redis_simple_add_gtest_suite(ListEncodingTest)
redis_simple_add_gtest_suite(SetTest)
redis_simple_add_gtest_suite(HashTest)
redis_simple_add_gtest_suite(HashEncodingTest)
redis_simple_add_gtest_suite(ZSetListPackTest)
redis_simple_add_gtest_suite(ZSetSkiplistTest)
redis_simple_add_gtest_suite(ZSetSkiplistStandaloneTest)
redis_simple_add_gtest_suite(ZSetTest)
redis_simple_add_gtest_suite(ZSetEncodingTest)
redis_simple_add_gtest_suite(RespParserTest)
redis_simple_add_gtest_suite(ConnectionTest)
redis_simple_add_gtest_suite(ClientTest)
redis_simple_add_gtest_suite(ProtectedModeTest)
redis_simple_add_gtest_suite(RequestParserTest)
redis_simple_add_gtest_suite(TcpTest)

redis_simple_add_executable(redis_simple_integration_tcp_server "integration/tcp/server_test.cpp")
target_link_libraries(redis_simple_integration_tcp_server PRIVATE redis_simple_core)

redis_simple_add_executable(redis_simple_integration_tcp_client "integration/tcp/client_test.cpp")
target_link_libraries(redis_simple_integration_tcp_client PRIVATE redis_simple_core)

redis_simple_add_executable(
  redis_simple_integration_key_client
  "integration/commands/key_client_test.cpp"
)
target_link_libraries(redis_simple_integration_key_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_connection_client
  "integration/commands/connection_client_test.cpp"
)
target_link_libraries(redis_simple_integration_connection_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_string_client
  "integration/commands/string_client_test.cpp"
)
target_link_libraries(redis_simple_integration_string_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_set_client
  "integration/commands/set_client_test.cpp"
)
target_link_libraries(redis_simple_integration_set_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_list_client
  "integration/commands/list_client_test.cpp"
)
target_link_libraries(redis_simple_integration_list_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_zset_client
  "integration/commands/zset_client_test.cpp"
)
target_link_libraries(redis_simple_integration_zset_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_hash_client
  "integration/commands/hash_client_test.cpp"
)
target_link_libraries(redis_simple_integration_hash_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_transaction_client
  "integration/commands/transaction_client_test.cpp"
)
target_link_libraries(redis_simple_integration_transaction_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_aof_client
  "integration/aof_client_test.cpp"
)
target_link_libraries(redis_simple_integration_aof_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_rdb_client
  "integration/rdb_client_test.cpp"
)
target_link_libraries(redis_simple_integration_rdb_client PRIVATE redis_simple_cli)

redis_simple_add_executable(
  redis_simple_integration_io_threads_client
  "integration/io_threads_client_test.cpp"
)
target_link_libraries(redis_simple_integration_io_threads_client PRIVATE redis_simple_cli)

add_test(
  NAME redis_simple_integration_tcp
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_tcp.sh"
    $<TARGET_FILE:redis_simple_integration_tcp_server>
    $<TARGET_FILE:redis_simple_integration_tcp_client>
)

add_test(
  NAME redis_simple_integration_server_lifecycle
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_server_lifecycle_test.sh"
    $<TARGET_FILE:redis_simple>
)

foreach(variant IN ITEMS "" _io_threads)
  add_test(
    NAME redis_simple_integration_aof${variant}
    COMMAND
      /bin/sh
      "${CMAKE_SOURCE_DIR}/scripts/run_aof_integration_test.sh"
      $<TARGET_FILE:redis_simple>
      $<TARGET_FILE:redis_simple_integration_aof_client>
  )
endforeach()
foreach(variant IN ITEMS "" _io_threads)
  add_test(
    NAME redis_simple_integration_rdb${variant}
    COMMAND
      /bin/sh
      "${CMAKE_SOURCE_DIR}/scripts/run_rdb_integration_test.sh"
      $<TARGET_FILE:redis_simple>
      $<TARGET_FILE:redis_simple_integration_rdb_client>
  )
endforeach()
set_tests_properties(
  redis_simple_integration_rdb_io_threads
  PROPERTIES ENVIRONMENT "REDIS_SIMPLE_SERVER_ARGS=--io-threads 4"
)
# Rewrites fork while the I/O threads run, which the fork hooks pause.
set_tests_properties(
  redis_simple_integration_aof_io_threads
  PROPERTIES ENVIRONMENT "REDIS_SIMPLE_SERVER_ARGS=--io-threads 4"
)

add_test(
  NAME redis_simple_integration_command_key
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_key_client>
)

add_test(
  NAME redis_simple_integration_command_connection
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_connection_client>
)

add_test(
  NAME redis_simple_integration_command_string
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_string_client>
)

add_test(
  NAME redis_simple_integration_command_set
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_set_client>
)

add_test(
  NAME redis_simple_integration_command_list
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_list_client>
)

add_test(
  NAME redis_simple_integration_command_zset
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_zset_client>
)

add_test(
  NAME redis_simple_integration_command_hash
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_hash_client>
)

add_test(
  NAME redis_simple_integration_command_transaction
  COMMAND
    /bin/sh
    "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
    $<TARGET_FILE:redis_simple>
    $<TARGET_FILE:redis_simple_integration_transaction_client>
)

# Many concurrent pipelined clients, against a server without I/O threads and
# one whose batches are large enough to share across them.
foreach(variant IN ITEMS plain io_threads)
  add_test(
    NAME redis_simple_integration_concurrent_clients_${variant}
    COMMAND
      /bin/sh
      "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
      $<TARGET_FILE:redis_simple>
      $<TARGET_FILE:redis_simple_integration_io_threads_client>
  )
  set_tests_properties(
    redis_simple_integration_concurrent_clients_${variant}
    PROPERTIES
      LABELS integration
      RESOURCE_LOCK redis_simple_port_8080
  )
endforeach()
set_tests_properties(
  redis_simple_integration_concurrent_clients_io_threads
  PROPERTIES ENVIRONMENT "REDIS_SIMPLE_SERVER_ARGS=--io-threads 4"
)

# The command families again against a server with I/O threads, which defers
# every read and write to batches handled before the event loop waits.
foreach(family IN ITEMS key connection string set list zset hash transaction)
  add_test(
    NAME redis_simple_integration_command_${family}_io_threads
    COMMAND
      /bin/sh
      "${CMAKE_SOURCE_DIR}/scripts/run_integration_commands.sh"
      $<TARGET_FILE:redis_simple>
      $<TARGET_FILE:redis_simple_integration_${family}_client>
  )
  set_tests_properties(
    redis_simple_integration_command_${family}_io_threads
    PROPERTIES
      ENVIRONMENT "REDIS_SIMPLE_SERVER_ARGS=--io-threads 4"
      LABELS integration
      RESOURCE_LOCK redis_simple_port_8080
  )
endforeach()

set_tests_properties(
  redis_simple_integration_tcp
  redis_simple_integration_server_lifecycle
  redis_simple_integration_aof
  redis_simple_integration_aof_io_threads
  redis_simple_integration_rdb
  redis_simple_integration_rdb_io_threads
  redis_simple_integration_command_connection
  redis_simple_integration_command_key
  redis_simple_integration_command_string
  redis_simple_integration_command_set
  redis_simple_integration_command_list
  redis_simple_integration_command_zset
  redis_simple_integration_command_hash
  redis_simple_integration_command_transaction
  PROPERTIES
    LABELS integration
)
set_tests_properties(
  redis_simple_integration_tcp
  redis_simple_integration_command_connection
  redis_simple_integration_command_key
  redis_simple_integration_command_string
  redis_simple_integration_command_set
  redis_simple_integration_command_list
  redis_simple_integration_command_zset
  redis_simple_integration_command_hash
  PROPERTIES RESOURCE_LOCK redis_simple_port_8080
)
set_tests_properties(
  redis_simple_integration_server_lifecycle
  PROPERTIES RESOURCE_LOCK redis_simple_port_18080
)
set_tests_properties(
  redis_simple_integration_aof
  redis_simple_integration_aof_io_threads
  PROPERTIES RESOURCE_LOCK redis_simple_port_18081
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark's tests" FORCE)
add_subdirectory("third_party/benchmark")

redis_simple_collect_sources(REDIS_SIMPLE_BENCHMARK_SOURCES benchmarks)
# Replaces operator new and delete, so it is built only on request.
redis_simple_exclude_sources(REDIS_SIMPLE_BENCHMARK_SOURCES
  "/benchmarks/mimalloc_new_delete\\.cpp$")

redis_simple_add_executable(redis_simple_benchmarks ${REDIS_SIMPLE_BENCHMARK_SOURCES})
target_link_libraries(
  redis_simple_benchmarks
  PRIVATE
    redis_simple_core
    benchmark::benchmark
)
if(REDIS_SIMPLE_MIMALLOC_ALLOWED AND REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC)
  target_sources(redis_simple_benchmarks
    PRIVATE "${CMAKE_SOURCE_DIR}/benchmarks/mimalloc_new_delete.cpp")
  target_link_libraries(redis_simple_benchmarks PRIVATE mimalloc-static)
endif()

if(REDIS_SIMPLE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "Fuzz targets require Clang or AppleClang")
  endif()

  add_library(redis_simple_fuzz_core STATIC ${REDIS_SIMPLE_CORE_SOURCES})
  redis_simple_configure_target(redis_simple_fuzz_core)
  target_link_libraries(redis_simple_fuzz_core PUBLIC Threads::Threads)
  target_compile_options(
    redis_simple_fuzz_core
    PRIVATE
      -fsanitize=fuzzer-no-link,address,undefined
      -fno-omit-frame-pointer
  )

  function(redis_simple_add_fuzzer target source)
    redis_simple_add_executable(${target} "${source}")
    target_compile_options(
      ${target}
      PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${target} PRIVATE redis_simple_fuzz_core)
    add_test(
      NAME redis_simple_fuzz_${target}
      COMMAND
        ${target}
        -runs=2000
        -max_len=4096
        -seed=1
        -timeout=5
    )
    set_tests_properties(
      redis_simple_fuzz_${target}
      PROPERTIES
        LABELS fuzz
        TIMEOUT 60
    )
  endfunction()

  set(
    REDIS_SIMPLE_FUZZ_TARGETS
    request_parser_fuzzer
    listpack_fuzzer
    quicklist_fuzzer
    list_fuzzer
    set_fuzzer
    hash_fuzzer
    zset_fuzzer
    dict_fuzzer
    skiplist_fuzzer
    intset_fuzzer
    dynamic_buffer_fuzzer
    reply_buffer_fuzzer
    db_expiration_fuzzer
    aof_fuzzer
    rdb_fuzzer
    event_loop_fuzzer
  )
  foreach(fuzz_target IN LISTS REDIS_SIMPLE_FUZZ_TARGETS)
    redis_simple_add_fuzzer(${fuzz_target} "fuzz/${fuzz_target}.cpp")
  endforeach()
  add_custom_target(redis_simple_fuzzers DEPENDS ${REDIS_SIMPLE_FUZZ_TARGETS})
endif()
