# Host (PC) build: font converter, font inspector and the reference renderer.
#
#   cmake -S host -B build-host && cmake --build build-host
#
# Builds the same core/ sources as the firmware, for the PC:
#   sf2conv   .sf2 -> .fsf font compiler (font reader only)
#   fsfinfo   lists the contents of a .fsf (font reader only)
#   fsrender  renders a MIDI file to WAV with the full synth core
#   fsspeak   speaks English text or phonemes into a WAV (core/fs_speech.c);
#             host/get_cmudict.sh fetches the dictionary text input needs
# The core is split into two libraries so the font tools do not pull in
# the synth. fs_config.h defaults apply; override with -D options as on
# the device.
#
# Tests (tests/, run with CTest):
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
# Configure with -DFS_SANITIZE=ON to build everything with AddressSanitizer
# and UndefinedBehaviorSanitizer (use a separate build directory).
#
cmake_minimum_required(VERSION 3.13)
project(pico_fsynth_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(FS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(FS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(FS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../core)

# Compiled font format: generator tables, CRC and the .fsf reader
add_library(fs_core_font STATIC
    ${FS_CORE_DIR}/fs_gen.c
    ${FS_CORE_DIR}/fs_crc32.c
    ${FS_CORE_DIR}/fs_font.c
)
target_include_directories(fs_core_font PUBLIC ${FS_CORE_DIR})
target_compile_options(fs_core_font PUBLIC -Wall -Wextra)

# The synthesizer itself: voices, effects, mixing and the MIDI file player
add_library(fs_core_synth STATIC
    ${FS_CORE_DIR}/fs_math.c
    ${FS_CORE_DIR}/fs_stream.c
    ${FS_CORE_DIR}/fs_voice.c
    ${FS_CORE_DIR}/fs_effects.c
    ${FS_CORE_DIR}/fs_synth.c
    ${FS_CORE_DIR}/fs_smf.c
    ${FS_CORE_DIR}/fs_speech.c
)
target_link_libraries(fs_core_synth PUBLIC fs_core_font m)

# fsrender -t uses a second thread to mirror the Pico's dual core render
add_executable(fsrender fsrender.c)
find_package(Threads REQUIRED)
target_link_libraries(fsrender fs_core_synth Threads::Threads)

add_executable(fsspeak fsspeak.c fs_text.c)
target_link_libraries(fsspeak fs_core_synth)

add_executable(sf2conv sf2conv.c)
target_link_libraries(sf2conv fs_core_font)

add_executable(fsfinfo fsfinfo.c)
target_link_libraries(fsfinfo fs_core_font)

enable_testing()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../tests tests)
