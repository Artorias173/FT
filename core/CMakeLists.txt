cmake_minimum_required(VERSION 3.22)
project(ftvita_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cœur portable : compilé tel quel pour Android (NDK) et pour Linux (tests).
add_library(vita_core STATIC
    src/emulator.cpp
    src/memory/memory.cpp
    src/cpu/interpreter.cpp
    src/cpu/interpreter_arm.cpp
    src/cpu/interpreter_thumb.cpp
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
    enable_language(C)
    # programs.c est aussi compilé nativement : il fournit les résultats attendus.
    add_executable(vita_core_tests tests/core_tests.cpp tests/guest/programs.c)
    target_link_libraries(vita_core_tests PRIVATE vita_core)
    add_test(NAME vita_core_tests COMMAND vita_core_tests)

    # Programmes invités : les mêmes sources C compilées pour un Cortex-A9,
    # en ARM et en Thumb-2, à plusieurs niveaux d'optimisation.
    find_program(VITA_GUEST_CLANG clang)
    find_program(VITA_GUEST_LLD ld.lld)
    if(VITA_GUEST_CLANG AND VITA_GUEST_LLD)
        set(guest_src ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest)
        set(guest_dir ${CMAKE_CURRENT_BINARY_DIR}/guest)
        set(guest_outputs)
        foreach(mode thumb arm)
            foreach(opt O0 O2 Os)
                set(name ${mode}_${opt})
                set(flags --target=armv7a-none-eabi -mcpu=cortex-a9 -mfloat-abi=soft -m${mode} -${opt}
                          -ffreestanding -fno-builtin -nostdlib -fno-stack-protector)
                add_custom_command(
                    OUTPUT ${guest_dir}/${name}.elf
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${guest_dir}
                    COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/programs.c -o ${guest_dir}/${name}_programs.o
                    COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/guest_start.c -o ${guest_dir}/${name}_start.o
                    COMMAND ${VITA_GUEST_LLD} -static -nostdlib -e _start -Ttext=0x81000000 -z max-page-size=4096
                            ${guest_dir}/${name}_start.o ${guest_dir}/${name}_programs.o -o ${guest_dir}/${name}.elf
                    DEPENDS ${guest_src}/programs.c ${guest_src}/programs.h ${guest_src}/guest_start.c
                    VERBATIM)
                list(APPEND guest_outputs ${guest_dir}/${name}.elf)
            endforeach()
        endforeach()
        add_custom_target(vita_guest_programs DEPENDS ${guest_outputs})
        add_dependencies(vita_core_tests vita_guest_programs)
        target_compile_definitions(vita_core_tests PRIVATE VITA_GUEST_DIR="${guest_dir}")
    else()
        message(STATUS "clang ou ld.lld introuvable : tests sur programmes invités désactivés")
    endif()
endif()
