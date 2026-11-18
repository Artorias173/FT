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
    src/cpu/interpreter_vfp.cpp
    src/cpu/interpreter_neon.cpp
    src/cpu/fp.cpp
    src/loader/elf_loader.cpp
    src/hle/kernel.cpp
    src/gpu/renderer.cpp
)
target_include_directories(vita_core PUBLIC include)
# Les changements de mode d'arrondi de l'hôte doivent être respectés.
set_source_files_properties(src/cpu/fp.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math")
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
    add_executable(vita_core_tests tests/core_tests.cpp tests/fp_tests.cpp tests/guest/programs.c)
    target_link_libraries(vita_core_tests PRIVATE vita_core)
    add_test(NAME vita_core_tests COMMAND vita_core_tests)

    # Programmes invités : les mêmes sources C compilées pour un Cortex-A9.
    # Format : mode;optimisation;flottants (soft = sans VFP, vfp = VFPv3 matériel).
    target_sources(vita_core_tests PRIVATE tests/guest/programs_float.c tests/guest/programs_neon.c)
    set_source_files_properties(tests/guest/programs_float.c tests/guest/programs_neon.c
                                PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

    find_program(VITA_GUEST_CLANG clang)
    find_program(VITA_GUEST_LLD ld.lld)
    if(VITA_GUEST_CLANG AND VITA_GUEST_LLD)
        set(guest_src ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest)
        set(guest_dir ${CMAKE_CURRENT_BINARY_DIR}/guest)
        set(guest_outputs)
        foreach(spec thumb:O0:soft thumb:O2:soft thumb:Os:soft arm:O0:soft arm:O2:soft arm:Os:soft
                     thumb:O0:vfp thumb:O2:vfp arm:O2:vfp
                     thumb:O0:neon thumb:O2:neon thumb:O3:neon arm:O2:neon arm:O3:neon)
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 mode)
            list(GET parts 1 opt)
            list(GET parts 2 fp)
            set(name ${mode}_${opt})
            set(flags --target=armv7a-none-eabi -mcpu=cortex-a9 -m${mode} -${opt}
                      -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-math-errno -ffp-contract=off)
            set(objects ${guest_dir}/${name}_${fp}_start.o ${guest_dir}/${name}_${fp}_programs.o)
            set(extra_commands)
            if(fp STREQUAL "vfp")
                set(name ${name}_vfp)
                list(APPEND flags -mfloat-abi=hard -mfpu=vfpv3 -DVITA_GUEST_FLOAT)
                list(APPEND objects ${guest_dir}/${name}_float.o)
                set(extra_commands COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/programs_float.c
                                           -o ${guest_dir}/${name}_float.o)
            elseif(fp STREQUAL "neon")
                set(name ${name}_neon)
                list(APPEND flags -mfloat-abi=hard -mfpu=neon-fp16 -DVITA_GUEST_FLOAT -DVITA_GUEST_NEON)
                list(APPEND objects ${guest_dir}/${name}_float.o ${guest_dir}/${name}_neon.o)
                set(extra_commands
                    COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/programs_float.c -o ${guest_dir}/${name}_float.o
                    COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/programs_neon.c -o ${guest_dir}/${name}_neon.o)
            else()
                list(APPEND flags -mfloat-abi=soft)
            endif()
            add_custom_command(
                OUTPUT ${guest_dir}/${name}.elf
                COMMAND ${CMAKE_COMMAND} -E make_directory ${guest_dir}
                COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/programs.c -o ${guest_dir}/${mode}_${opt}_${fp}_programs.o
                COMMAND ${VITA_GUEST_CLANG} ${flags} -c ${guest_src}/guest_start.c -o ${guest_dir}/${mode}_${opt}_${fp}_start.o
                ${extra_commands}
                COMMAND ${VITA_GUEST_LLD} -static -nostdlib -e _start -Ttext=0x81000000 -z max-page-size=4096
                        ${objects} -o ${guest_dir}/${name}.elf
                DEPENDS ${guest_src}/programs.c ${guest_src}/programs_float.c ${guest_src}/programs_neon.c
                        ${guest_src}/programs.h
                        ${guest_src}/guest_start.c
                VERBATIM)
            list(APPEND guest_outputs ${guest_dir}/${name}.elf)
        endforeach()
        add_custom_target(vita_guest_programs DEPENDS ${guest_outputs})
        add_dependencies(vita_core_tests vita_guest_programs)
        target_compile_definitions(vita_core_tests PRIVATE VITA_GUEST_DIR="${guest_dir}")
    else()
        message(STATUS "clang ou ld.lld introuvable : tests sur programmes invités désactivés")
    endif()
endif()
