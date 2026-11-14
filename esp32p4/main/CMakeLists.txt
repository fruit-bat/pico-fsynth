# The firmware component: the ESP32-P4 specific files here plus the shared,
# portable synth core from ../../core.

set(FS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../core)
set(FS_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../firmware)   # midi_parse.c/h, midi_in.h and settings.h only

# Only those firmware headers are put on the include path (copied, and
# re-copied when they change): the RP2350's tusb_config.h must not be seen
# by TinyUSB here.
set(FS_FW_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/fw_include)
foreach(h midi_in.h midi_parse.h settings.h)
    configure_file(${FS_FW_DIR}/${h} ${FS_FW_INCLUDE}/${h} COPYONLY)
endforeach()

# Optional MIDI file embedded and played at boot:
#   idf.py -DFS_DEMO_MIDI=/path/to/song.mid build
set(FS_DEMO_MIDI "" CACHE FILEPATH "Optional MIDI file embedded and played at boot")
set(FS_DEMO_C ${CMAKE_CURRENT_BINARY_DIR}/demo_midi.c)
if(FS_DEMO_MIDI)
    file(READ ${FS_DEMO_MIDI} FS_DEMO_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FS_DEMO_BYTES "${FS_DEMO_HEX}")
    file(WRITE ${FS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t fs_demo_midi[] = {${FS_DEMO_BYTES}};\nconst size_t fs_demo_midi_size = sizeof(fs_demo_midi);\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FS_DEMO_MIDI})
else()
    file(WRITE ${FS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t fs_demo_midi[1] = {0};\nconst size_t fs_demo_midi_size = 0;\n")
endif()

set(FS_CORE_HOT
    ${CMAKE_CURRENT_LIST_DIR}/bench.c
    ${FS_CORE_DIR}/fs_voice.c
    ${FS_CORE_DIR}/fs_effects.c
    ${FS_CORE_DIR}/fs_synth.c
    ${FS_CORE_DIR}/fs_stream.c
    ${FS_CORE_DIR}/fs_codec.c
)

idf_component_register(
    SRCS
        main.c
        audio_i2s.c
        midi_in.c
        console.c
        settings.c
        ${FS_FW_DIR}/midi_parse.c
        ${FS_CORE_HOT}
        ${FS_CORE_DIR}/fs_gen.c
        ${FS_CORE_DIR}/fs_crc32.c
        ${FS_CORE_DIR}/fs_font.c
        ${FS_CORE_DIR}/fs_math.c
        ${FS_CORE_DIR}/fs_smf.c
        ${FS_DEMO_C}
    INCLUDE_DIRS . ${FS_CORE_DIR} ${FS_FW_INCLUDE}
    PRIV_REQUIRES esp_driver_i2s esp_driver_uart esp_partition esp_psram esp_timer nvs_flash vfs
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FS_SAMPLE_RATE=${CONFIG_FSYNTH_SAMPLE_RATE}
    FS_MAX_VOICES=${CONFIG_FSYNTH_MAX_VOICES}
    FS_REGION_INDEX_SIZE=${CONFIG_FSYNTH_REGION_INDEX_SIZE}
    FS_INTERP_TABLE_BITS=${CONFIG_FSYNTH_INTERP_TABLE_BITS}
)
if(CONFIG_FSYNTH_USB_MIDI)
    target_sources(${COMPONENT_LIB} PRIVATE usb_midi.c)
endif()
if(CONFIG_FSYNTH_EFFECTS_IN_PSRAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FS_EFFECTS_EXTERNAL=1)
endif()
if(CONFIG_FSYNTH_RENDER_TWO_PASS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FS_RENDER_TWO_PASS=1)
endif()
# The region index is only read at note-on: PSRAM is fine
target_compile_definitions(${COMPONENT_LIB} PRIVATE FS_REGION_INDEX_EXTERNAL=1)

# The synth core is float heavy: keep it fast. Fused multiply-add is on by
# default here (faster on the P4, unlike the RP2350; see Kconfig.projbuild).
set(FS_CORE_OPTIONS "-O3;-fno-math-errno")
if(NOT CONFIG_FSYNTH_FP_CONTRACT)
    list(APPEND FS_CORE_OPTIONS "-ffp-contract=off")
endif()
set_source_files_properties(${FS_CORE_HOT} PROPERTIES COMPILE_OPTIONS "${FS_CORE_OPTIONS}")
