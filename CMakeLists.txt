cmake_minimum_required(VERSION 3.20)

project(gdb_cli_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COMPILE_COMMANDS ON)
add_executable(gdb-agent
    src/main.cpp
    src/cli.cpp
    src/common/json.cpp
    src/common/string_utils.cpp
    src/evidence/evidence_store.cpp
    src/gdb/gdb_process.cpp
    src/gdb/gdb_session.cpp
    src/gdb/mi_utils.cpp
    src/report/report.cpp
    src/task/debug_task.cpp
    src/workflow/crash_workflow.cpp
)

target_compile_options(gdb-agent PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

install(TARGETS gdb-agent RUNTIME DESTINATION bin)

add_executable(segfault
    examples/segfault.cpp
)

target_compile_options(segfault PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -g
    -O0
)

enable_testing()

add_test(
    NAME segfault_demo_check
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_segfault_demo.sh --check-only
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
