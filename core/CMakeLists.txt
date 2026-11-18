cmake_minimum_required(VERSION 3.22)
project(ftvita_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cœur portable : compilé tel quel pour Android (NDK) et pour Linux (tests).
add_library(vita_core STATIC
    src/emulator.cpp
    src/memory/memory.cpp
    src/cpu/interpreter.cpp
    src/loader/elf_loader.cpp
    src/hle/kernel.cpp
    src/gpu/renderer.cpp
)
target_include_directories(vita_core PUBLIC include)
target_compile_options(vita_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(vita_core PUBLIC Threads::Threads)
if(ANDROID)
    target_link_libraries(vita_core PUBLIC log)
endif()

option(VITA_BUILD_TESTS "Compiler les tests du cœur" ${PROJECT_IS_TOP_LEVEL})
if(VITA_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(vita_core_tests tests/core_tests.cpp)
    target_link_libraries(vita_core_tests PRIVATE vita_core)
    add_test(NAME vita_core_tests COMMAND vita_core_tests)
endif()
