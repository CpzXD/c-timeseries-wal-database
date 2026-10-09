cmake_minimum_required(VERSION 3.21)
project(tsdb_c VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(tsdb_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

set(TSDB_CORE_SOURCES
    src/record.c
    src/wal_format.c
    src/crc32.c
    src/wal.c
    src/index.c
    src/tsdb.c
)
add_library(tsdb_core STATIC ${TSDB_CORE_SOURCES})
target_include_directories(tsdb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
tsdb_warnings(tsdb_core)

add_executable(wal_tests tests/test_wal.c)
target_link_libraries(wal_tests PRIVATE tsdb_core)
tsdb_warnings(wal_tests)

add_executable(wal_engine_tests tests/test_wal_engine.c)
target_link_libraries(wal_engine_tests PRIVATE tsdb_core)
tsdb_warnings(wal_engine_tests)

add_executable(wal_safety_tests tests/test_wal_safety.c)
target_link_libraries(wal_safety_tests PRIVATE tsdb_core)
tsdb_warnings(wal_safety_tests)

add_executable(wal_fault_tests
    tests/test_wal_fault.c
    src/record.c
    src/wal_format.c
    src/crc32.c
    src/wal.c
)
target_include_directories(wal_fault_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(wal_fault_tests PRIVATE TSDB_WAL_FAULT_TEST)
tsdb_warnings(wal_fault_tests)

add_executable(wal_crash_tests tests/test_wal_crash.c ${TSDB_CORE_SOURCES})
target_include_directories(wal_crash_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(wal_crash_tests PRIVATE TSDB_WAL_FAULT_TEST)
tsdb_warnings(wal_crash_tests)

add_executable(query_tests tests/test_query.c)
target_link_libraries(query_tests PRIVATE tsdb_core)
tsdb_warnings(query_tests)

add_executable(query_model_tests tests/test_query_model.c)
target_link_libraries(query_model_tests PRIVATE tsdb_core)
tsdb_warnings(query_model_tests)

add_executable(tsdb src/main.c)
target_link_libraries(tsdb PRIVATE tsdb_core)
tsdb_warnings(tsdb)

add_executable(record_tests tests/test_record.c)
target_link_libraries(record_tests PRIVATE tsdb_core)
tsdb_warnings(record_tests)

enable_testing()
add_test(NAME setup_smoke COMMAND tsdb --version)
add_test(NAME record_format COMMAND record_tests)
add_test(NAME wal_file_smoke COMMAND wal_tests)
add_test(NAME wal_engine COMMAND wal_engine_tests)
add_test(NAME wal_safety COMMAND wal_safety_tests)
add_test(NAME wal_faults COMMAND wal_fault_tests)
add_test(NAME wal_process_crash COMMAND wal_crash_tests)
set_tests_properties(wal_process_crash PROPERTIES TIMEOUT 30)
add_test(NAME query_index COMMAND query_tests)
add_test(NAME query_model COMMAND query_model_tests)
