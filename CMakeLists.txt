cmake_minimum_required(VERSION 3.20)
project(AsteriaRT VERSION 0.1.0 LANGUAGES CXX)

option(ASTERIA_BUILD_TESTS "Build AsteriaRT tests" ON)
option(ASTERIA_ENABLE_WERROR "Treat warnings as errors" OFF)

add_library(asteria_core
    src/loader/elf64.cpp
    src/runtime/symbol_registry.cpp
    src/memory/guest_address_space.cpp
    src/gpu/null_backend.cpp
    src/gpu/pm4.cpp
    src/runtime/runtime.cpp
)
add_library(AsteriaRT::core ALIAS asteria_core)

target_include_directories(asteria_core PUBLIC include)
target_compile_features(asteria_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(asteria_core PRIVATE /W4 /permissive-)
    if(ASTERIA_ENABLE_WERROR)
        target_compile_options(asteria_core PRIVATE /WX)
    endif()
else()
    target_compile_options(asteria_core PRIVATE -Wall -Wextra -Wpedantic)
    if(ASTERIA_ENABLE_WERROR)
        target_compile_options(asteria_core PRIVATE -Werror)
    endif()
endif()

add_executable(asteria app/main.cpp)
target_link_libraries(asteria PRIVATE AsteriaRT::core)

if(ASTERIA_BUILD_TESTS)
    enable_testing()
    add_executable(asteria_tests
        tests/main.cpp
        tests/elf64_tests.cpp
        tests/symbol_registry_tests.cpp
        tests/guest_address_space_tests.cpp
        tests/gpu_backend_tests.cpp
        tests/pm4_tests.cpp
    )
    target_link_libraries(asteria_tests PRIVATE AsteriaRT::core)
    add_test(NAME asteria_tests COMMAND asteria_tests)
endif()
