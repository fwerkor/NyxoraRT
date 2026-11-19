cmake_minimum_required(VERSION 3.20)
project(NyxoraRT VERSION 0.1.0 LANGUAGES CXX)

option(NYXORA_BUILD_TESTS "Build NyxoraRT tests" ON)
option(NYXORA_ENABLE_WERROR "Treat warnings as errors" OFF)
option(NYXORA_ENABLE_COVERAGE "Enable source coverage instrumentation" OFF)
option(NYXORA_ENABLE_VULKAN_DIAGNOSTICS "Build headless Vulkan scalar diagnostic runner" OFF)

if(NYXORA_ENABLE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "NYXORA_ENABLE_COVERAGE requires GCC or Clang")
endif()

find_package(Threads REQUIRED)

include(FetchContent)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(Zydis)

add_library(nyxora_core
    src/loader/elf64.cpp
    src/loader/dynamic.cpp
    src/runtime/symbol_registry.cpp
    src/runtime/linker.cpp
    src/runtime/late_imports.cpp
    src/runtime/hle_registry.cpp
    src/runtime/kernel_services.cpp
    src/runtime/fault.cpp
    src/runtime/cpu_patches.cpp
    src/runtime/guest_thread.cpp
    src/runtime/thread_manager.cpp
    src/runtime/native_thread.cpp
    src/runtime/tls.cpp
    src/memory/guest_address_space.cpp
    src/memory/native_arena.cpp
    src/gpu/backend.cpp
    src/gpu/null_backend.cpp
    src/gpu/pm4.cpp
    src/gpu/pm4_trace.cpp
    src/gpu/guest_resources.cpp
    src/gpu/shader/isa.cpp
    src/gpu/shader/scalar_decode.cpp
    src/gpu/shader/scalar_cfg.cpp
    src/gpu/shader/spirv.cpp
    src/gpu/shader/scalar_exec.cpp
    src/runtime/runtime.cpp
    src/hle/libkernel.cpp
)
add_library(NyxoraRT::core ALIAS nyxora_core)

target_include_directories(nyxora_core PUBLIC include)
target_compile_features(nyxora_core PUBLIC cxx_std_20)
target_link_libraries(nyxora_core PUBLIC Threads::Threads PRIVATE Zydis)

if(NYXORA_ENABLE_COVERAGE)
    target_compile_options(nyxora_core PUBLIC --coverage -O0 -g)
    target_link_options(nyxora_core INTERFACE --coverage)
endif()

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

if(NYXORA_ENABLE_VULKAN_DIAGNOSTICS)
    find_package(Vulkan REQUIRED)
    add_executable(nyxora_vulkan_scalar tools/vulkan_scalar_runner.cpp)
    target_compile_features(nyxora_vulkan_scalar PRIVATE cxx_std_20)
    target_link_libraries(nyxora_vulkan_scalar PRIVATE Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(nyxora_vulkan_scalar PRIVATE /W4 /permissive-)
        if(NYXORA_ENABLE_WERROR)
            target_compile_options(nyxora_vulkan_scalar PRIVATE /WX)
        endif()
    else()
        target_compile_options(nyxora_vulkan_scalar PRIVATE -Wall -Wextra -Wpedantic)
        if(NYXORA_ENABLE_WERROR)
            target_compile_options(nyxora_vulkan_scalar PRIVATE -Werror)
        endif()
    endif()
endif()

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
        tests/cpu_patch_tests.cpp
        tests/guest_thread_tests.cpp
        tests/thread_manager_tests.cpp
        tests/runtime_tests.cpp
        tests/module_lifecycle_tests.cpp
        tests/guest_address_space_tests.cpp
        tests/native_arena_tests.cpp
        tests/native_thread_tests.cpp
        tests/tls_tests.cpp
        tests/gpu_backend_tests.cpp
        tests/pm4_tests.cpp
        tests/pm4_indexed_tests.cpp
        tests/pm4_trace_tests.cpp
        tests/gpu_guest_resources_tests.cpp
        tests/shader_isa_tests.cpp
        tests/shader_scalar_decode_tests.cpp
        tests/shader_scalar_cfg_tests.cpp
        tests/shader_spirv_tests.cpp
        tests/shader_scalar_exec_tests.cpp
    )
    target_link_libraries(nyxora_tests PRIVATE NyxoraRT::core)
    add_test(NAME nyxora_tests COMMAND nyxora_tests)

    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(NYXORA_SPIRV_VAL_EXECUTABLE spirv-val)
    if(Python3_Interpreter_FOUND AND NYXORA_SPIRV_VAL_EXECUTABLE)
        add_test(NAME nyxora_spirv_conformance
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/spirv_conformance.py
                    $<TARGET_FILE:nyxora> ${NYXORA_SPIRV_VAL_EXECUTABLE})
    endif()
endif()
