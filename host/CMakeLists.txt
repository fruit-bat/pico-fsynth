# Host (PC) build: font converter, tools and later the reference renderer.
#
#   cmake -S host -B build-host && cmake --build build-host
#
cmake_minimum_required(VERSION 3.13)
project(pico_fsynth_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(FS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../core)

add_library(fs_core_font STATIC
    ${FS_CORE_DIR}/fs_gen.c
    ${FS_CORE_DIR}/fs_crc32.c
    ${FS_CORE_DIR}/fs_font.c
)
target_include_directories(fs_core_font PUBLIC ${FS_CORE_DIR})
target_compile_options(fs_core_font PUBLIC -Wall -Wextra)

add_library(fs_core_synth STATIC
    ${FS_CORE_DIR}/fs_math.c
    ${FS_CORE_DIR}/fs_stream.c
    ${FS_CORE_DIR}/fs_voice.c
    ${FS_CORE_DIR}/fs_effects.c
    ${FS_CORE_DIR}/fs_synth.c
    ${FS_CORE_DIR}/fs_smf.c
)
target_link_libraries(fs_core_synth PUBLIC fs_core_font m)

add_executable(fsrender fsrender.c)
find_package(Threads REQUIRED)
target_link_libraries(fsrender fs_core_synth Threads::Threads)

add_executable(sf2conv sf2conv.c)
target_link_libraries(sf2conv fs_core_font)

add_executable(fsfinfo fsfinfo.c)
target_link_libraries(fsfinfo fs_core_font)
