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
    src/replay/replay_plan.cpp
    src/report/report.cpp
    src/task/debug_task.cpp
    src/workflow/crash_workflow.cpp
    src/workflow/hypothesis.cpp
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

add_executable(capability_fixture
    examples/capability_fixture.cpp
)

target_compile_options(capability_fixture PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -g
    -O0
)

add_executable(mi_summary_tests
    tests/mi_summary_tests.cpp
    src/common/string_utils.cpp
    src/evidence/evidence_store.cpp
    src/gdb/mi_utils.cpp
)

target_compile_options(mi_summary_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(replay_plan_tests
    tests/replay_plan_tests.cpp
    src/common/json.cpp
    src/common/string_utils.cpp
    src/replay/replay_plan.cpp
)

target_compile_options(replay_plan_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(hypothesis_assertion_tests
    tests/hypothesis_assertion_tests.cpp
    src/common/string_utils.cpp
    src/workflow/hypothesis.cpp
)

target_compile_options(hypothesis_assertion_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(task_parser_tests
    tests/task_parser_tests.cpp
    src/common/json.cpp
    src/common/string_utils.cpp
    src/task/debug_task.cpp
)

target_compile_options(task_parser_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()

add_test(
    NAME segfault_demo_check
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_segfault_demo.sh --check-only
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME daemon_action_flow
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_daemon_action_flow.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME core_dump_mode
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_core_dump_mode.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME edge_case_flow
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_edge_cases.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME capability_matrix_flow
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_capability_matrix.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME catchpoint_matrix_flow
    COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/scripts/smoke_catchpoint_matrix.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME mi_summary_tests
    COMMAND mi_summary_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME replay_plan_tests
    COMMAND replay_plan_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME hypothesis_assertion_tests
    COMMAND hypothesis_assertion_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME task_parser_tests
    COMMAND task_parser_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
