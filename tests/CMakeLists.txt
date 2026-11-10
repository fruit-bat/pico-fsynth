# Host test suite, built by host/CMakeLists.txt and run with CTest:
#
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Each test_*.c is a self-contained program using the harness in test.h;
# a non-zero exit status is a failure.

set(FS_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../firmware)

# Pure core tests
foreach(name math smf stream synth speech)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} fs_core_synth)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The firmware's MIDI parsers (free of Pico SDK calls)
add_executable(test_midi_parse test_midi_parse.c ${FS_FW_DIR}/midi_parse.c)
target_include_directories(test_midi_parse PRIVATE ${FS_FW_DIR})
target_compile_options(test_midi_parse PRIVATE -Wall -Wextra)
add_test(NAME midi_parse COMMAND test_midi_parse)

# The host text to phoneme converter (writes a small dictionary to the
# build directory)
add_executable(test_text test_text.c ${CMAKE_CURRENT_LIST_DIR}/../host/fs_text.c)
target_include_directories(test_text PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../host)
target_link_libraries(test_text fs_core_synth)
add_test(NAME text COMMAND test_text WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# End to end: runs the sf2conv and fsrender binaries
add_executable(test_tools test_tools.c)
target_link_libraries(test_tools fs_core_font m)
add_dependencies(test_tools sf2conv fsrender)
add_test(NAME tools COMMAND test_tools $<TARGET_FILE:sf2conv> $<TARGET_FILE:fsrender>
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
