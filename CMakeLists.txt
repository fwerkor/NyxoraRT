cmake_minimum_required(VERSION 3.20)
project(NyxoraRT VERSION 0.1.0 LANGUAGES CXX)

option(NYXORA_BUILD_TESTS "Build NyxoraRT tests" ON)
option(NYXORA_ENABLE_WERROR "Treat warnings as errors" OFF)

find_package(Threads REQUIRED)

add_library(nyxora_core
    src/loader/elf64.cpp
    src/loader/dynamic.cpp
    src/runtime/symbol_registry.cpp
    src/runtime/linker.cpp
    src/runtime/late_imports.cpp
    src/runtime/hle_registry.cpp
    src/runtime/fault.cpp
    src/runtime/guest_thread.cpp
    src/runtime/native_thread.cpp
    src/runtime/tls.cpp
    src/memory/guest_address_space.cpp
    src/memory/native_arena.cpp
    src/gpu/null_backend.cpp
    src/gpu/pm4.cpp
    src/runtime/runtime.cpp
    src/hle/libkernel.cpp
)
add_library(NyxoraRT::core ALIAS nyxora_core)

target_include_directories(nyxora_core PUBLIC include)
target_compile_features(nyxora_core PUBLIC cxx_std_20)
target_link_libraries(nyxora_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(nyxora_core PRIVATE /W4 /permissive-)
    if(NYXORA_ENABLE_WERROR)
        target_compile_options(nyxora_core PRIVATE /WX)
    endif()
else()
    target_compile_options(nyxora_core PRIVATE -Wall -Wextra -Wpedantic)
    if(NYXORA_ENABLE_WERROR)
        target_compile_options(nyxora_core PRIVATE -Werror)
    endif()
endif()

add_executable(nyxora app/main.cpp)
target_link_libraries(nyxora PRIVATE NyxoraRT::core)

if(NYXORA_BUILD_TESTS)
    enable_testing()
    add_executable(nyxora_tests
        tests/main.cpp
        tests/elf64_tests.cpp
        tests/dynamic_tests.cpp
        tests/symbol_registry_tests.cpp
        tests/linker_tests.cpp
        tests/late_import_tests.cpp
        tests/hle_tests.cpp
        tests/fault_tests.cpp
        tests/guest_thread_tests.cpp
        tests/runtime_tests.cpp
        tests/guest_address_space_tests.cpp
        tests/native_arena_tests.cpp
        tests/native_thread_tests.cpp
        tests/tls_tests.cpp
        tests/gpu_backend_tests.cpp
        tests/pm4_tests.cpp
    )
    target_link_libraries(nyxora_tests PRIVATE NyxoraRT::core)
    add_test(NAME nyxora_tests COMMAND nyxora_tests)
endif()
