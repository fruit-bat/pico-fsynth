# pico-fsynth firmware (RP2350).
#
#   cmake -S . -B build -DFS_AUDIO_OUT=I2S      (or PWM)
#   cmake --build build
#
# The compiled font is flashed separately, see README.md.
#
cmake_minimum_required(VERSION 3.13)

if(NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_plus_16mb)
endif()

include(pico_sdk_import.cmake)

project(pico_fsynth C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

set(FS_AUDIO_OUT "I2S" CACHE STRING "Audio output: I2S or PWM")
set_property(CACHE FS_AUDIO_OUT PROPERTY STRINGS I2S PWM)
set(FS_SAMPLE_RATE 44100 CACHE STRING "Output sample rate in Hz")
set(FS_SYS_CLOCK_KHZ 250000 CACHE STRING "System clock in kHz (RP2350 is rated 150000; 250000 needed for 64 voices, tested OK)")
set(FS_MAX_VOICES 80 CACHE STRING "Maximum simultaneous voices (80 is about 70% of both cores at 250 MHz)")
option(FS_DUAL_CORE "Render voices on both cores" ON)
set(FS_FONT_FLASH_OFFSET 0x200000 CACHE STRING "Flash offset of the compiled font (firmware must fit below it)")

set(FS_I2S_DATA_PIN 12 CACHE STRING "I2S data out GPIO")
set(FS_I2S_CLOCK_PIN_BASE 13 CACHE STRING "I2S BCLK GPIO (LRCLK is the next GPIO)")
set(FS_PWM_PIN_L 20 CACHE STRING "PWM left GPIO (must be channel A of a slice)")
set(FS_PWM_PIN_R 21 CACHE STRING "PWM right GPIO (channel B of the same slice)")
set(FS_MIDI_UART_RX_PIN 5 CACHE STRING "MIDI in UART RX GPIO (UART1)")

# Waveshare Pico-Audio (PCM5101A version) with a Pico-pinout board plugged on
# top: DIN 26, BCK 27, LRCK 28. Overrides the I2S pins above. (The CS4344
# "Rev2.1" board uses other pins and needs MCLK, which is not supported.)
option(FS_PICO_AUDIO "I2S pins for a stacked Waveshare Pico-Audio (PCM5101A)" OFF)
if(FS_PICO_AUDIO)
    set(FS_AUDIO_OUT I2S)
    set(FS_I2S_DATA_PIN 26)
    set(FS_I2S_CLOCK_PIN_BASE 27)
endif()
message(STATUS "pico-fsynth: ${PICO_BOARD}, ${FS_AUDIO_OUT}, I2S data ${FS_I2S_DATA_PIN} clocks ${FS_I2S_CLOCK_PIN_BASE}/+1")

set(FS_DEMO_MIDI "" CACHE FILEPATH "Optional MIDI file embedded and played at boot until MIDI input arrives")

# ---------------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------------

set(FS_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/core)
set(FS_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/firmware)

add_executable(pico_fsynth
    ${FS_FW_DIR}/main.c
    ${FS_FW_DIR}/midi_in.c
    ${FS_FW_DIR}/settings.c
    ${FS_FW_DIR}/usb_descriptors.c
    ${FS_CORE_DIR}/fs_gen.c
    ${FS_CORE_DIR}/fs_crc32.c
    ${FS_CORE_DIR}/fs_font.c
    ${FS_CORE_DIR}/fs_math.c
    ${FS_CORE_DIR}/fs_stream.c
    ${FS_CORE_DIR}/fs_voice.c
    ${FS_CORE_DIR}/fs_effects.c
    ${FS_CORE_DIR}/fs_synth.c
    ${FS_CORE_DIR}/fs_smf.c
)

# Inner loop micro benchmarks, run at boot and printed every 30 s
option(FS_BENCH "Build the inner loop micro benchmarks into the firmware" OFF)
if(FS_BENCH)
    target_sources(pico_fsynth PRIVATE ${FS_FW_DIR}/bench.c)
    target_compile_definitions(pico_fsynth PRIVATE FS_BENCH=1)
endif()

if(FS_AUDIO_OUT STREQUAL "I2S")
    target_sources(pico_fsynth PRIVATE ${FS_FW_DIR}/audio_i2s.c)
    pico_generate_pio_header(pico_fsynth ${FS_FW_DIR}/audio_i2s.pio)
    target_link_libraries(pico_fsynth hardware_pio)
elseif(FS_AUDIO_OUT STREQUAL "PWM")
    target_sources(pico_fsynth PRIVATE ${FS_FW_DIR}/audio_pwm.c)
    target_link_libraries(pico_fsynth hardware_pwm)
else()
    message(FATAL_ERROR "FS_AUDIO_OUT must be I2S or PWM")
endif()

# Embed an optional demo MIDI file as a C array
set(FS_DEMO_C ${CMAKE_CURRENT_BINARY_DIR}/demo_midi.c)
if(FS_DEMO_MIDI)
    file(READ ${FS_DEMO_MIDI} FS_DEMO_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FS_DEMO_BYTES "${FS_DEMO_HEX}")
    file(WRITE ${FS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t fs_demo_midi[] = {${FS_DEMO_BYTES}};\nconst size_t fs_demo_midi_size = sizeof(fs_demo_midi);\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FS_DEMO_MIDI})
else()
    file(WRITE ${FS_DEMO_C} "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t fs_demo_midi[1] = {0};\nconst size_t fs_demo_midi_size = 0;\n")
endif()
target_sources(pico_fsynth PRIVATE ${FS_DEMO_C})

target_include_directories(pico_fsynth PRIVATE ${FS_CORE_DIR} ${FS_FW_DIR})

target_compile_definitions(pico_fsynth PRIVATE
    FS_AUDIO_OUT_${FS_AUDIO_OUT}=1
    FS_SAMPLE_RATE=${FS_SAMPLE_RATE}
    FS_SYS_CLOCK_KHZ=${FS_SYS_CLOCK_KHZ}
    FS_MAX_VOICES=${FS_MAX_VOICES}
    FS_DUAL_CORE=$<BOOL:${FS_DUAL_CORE}>
    FS_FONT_FLASH_OFFSET=${FS_FONT_FLASH_OFFSET}
    FS_I2S_DATA_PIN=${FS_I2S_DATA_PIN}
    FS_I2S_CLOCK_PIN_BASE=${FS_I2S_CLOCK_PIN_BASE}
    FS_PWM_PIN_L=${FS_PWM_PIN_L}
    FS_PWM_PIN_R=${FS_PWM_PIN_R}
    FS_MIDI_UART_RX_PIN=${FS_MIDI_UART_RX_PIN}
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_CORE1_STACK_SIZE=0x1000
)

# The synth core is float heavy: keep it fast.
# FS_FP_CONTRACT=OFF (the default) stops GCC fusing multiply + add into VFMA:
# on the RP2350 a VFMA costs more than a VMUL and a VADD (firmware/bench.c).
option(FS_FP_CONTRACT "Allow fused multiply-add in the synth core (OFF measured 7% faster on the RP2350)" OFF)
set(FS_CORE_OPTIONS "-O3;-fno-math-errno")
if(NOT FS_FP_CONTRACT)
    list(APPEND FS_CORE_OPTIONS "-ffp-contract=off")
    target_compile_definitions(pico_fsynth PRIVATE FS_FP_CONTRACT_OFF=1)
endif()
set_source_files_properties(
    ${FS_CORE_DIR}/fs_voice.c ${FS_CORE_DIR}/fs_effects.c ${FS_CORE_DIR}/fs_synth.c ${FS_CORE_DIR}/fs_stream.c
    ${FS_FW_DIR}/bench.c
    PROPERTIES COMPILE_OPTIONS "${FS_CORE_OPTIONS}"
)

target_link_libraries(pico_fsynth
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_flash
    hardware_dma
    hardware_uart
    hardware_clocks
    hardware_vreg
    tinyusb_device
)

pico_enable_stdio_usb(pico_fsynth 1)
pico_enable_stdio_uart(pico_fsynth 0)
pico_add_extra_outputs(pico_fsynth)
