cmake_minimum_required(VERSION 3.20)

project(MiniDB
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(minidb
    src/main.cpp
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/free_list.cpp
    src/storage/clock_replacer.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/slotted_page.cpp
    src/storage/heap_table.cpp
)

add_executable(disk_manager_persistence_test
    tests/disk_manager_persistence_test.cpp
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/free_list.cpp
)

add_executable(disk_manager_robustness_test
    tests/disk_manager_robustness_test.cpp
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/free_list.cpp
)

add_executable(buffer_pool_manager_test
    tests/buffer_pool_manager_test.cpp
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/free_list.cpp
    src/storage/clock_replacer.cpp
    src/storage/buffer_pool_manager.cpp
)

add_executable(slotted_page_test
    tests/slotted_page_test.cpp
    src/storage/page.cpp
    src/storage/slotted_page.cpp
)

add_executable(heap_table_test
    tests/heap_table_test.cpp
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/free_list.cpp
    src/storage/clock_replacer.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/slotted_page.cpp
    src/storage/heap_table.cpp
)

target_include_directories(minidb
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(disk_manager_persistence_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(disk_manager_robustness_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(buffer_pool_manager_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(slotted_page_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(heap_table_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(minidb
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(disk_manager_persistence_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(disk_manager_robustness_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(buffer_pool_manager_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(slotted_page_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(heap_table_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(
    NAME disk_manager_persistence_test
    COMMAND disk_manager_persistence_test
)

add_test(
    NAME disk_manager_robustness_test
    COMMAND disk_manager_robustness_test
)

add_test(
    NAME buffer_pool_manager_test
    COMMAND buffer_pool_manager_test
)

add_test(
    NAME slotted_page_test
    COMMAND slotted_page_test
)

add_test(
    NAME heap_table_test
    COMMAND heap_table_test
)
