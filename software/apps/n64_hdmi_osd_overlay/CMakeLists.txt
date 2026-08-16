# n64_hdmi_osd_overlay - N64 capture + OSD overlay, now with a third output
# mode: true 240p, 640x480 2x upscale, or 1280x720 2x/3x upscale, selectable
# at runtime from the OSD menu ("Video Mode" + "Save & Reboot").
#
# This build is STANDALONE (own project()/pico_sdk_init()/add_subdirectory of
# lib/pico_hdmi) for the same reason as apps/n64_diag_720p: the top-level
# build shares ONE pico_hdmi library target compiled without
# PICO_HDMI_RT_RUNTIME_MODE_ATTRS, but 720p CEA (positive sync, DI in back
# porch) needs that option so sync symbols / DI placement come from the mode
# descriptor at runtime. Same reasoning as
# apps/pico_hdmi_reference_bouncing_box_rt. The top-level CMakeLists no
# longer adds this app; build it separately.
#
# Build (separately from the main project):
#   cmake -S apps/n64_hdmi_osd_overlay \
#         -B build-osd-3modes -G Ninja -DPICO_SDK_PATH=$PICO_SDK_PATH
#   ninja -C build-osd-3modes
#   -> build-osd-3modes/n64_hdmi_osd_overlay.uf2

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

if(DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH} CACHE PATH "Path to the Raspberry Pi Pico SDK")
endif()
if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is not set (env var or -DPICO_SDK_PATH=...).")
endif()

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(n64_hdmi_osd_overlay C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Output mode the build DEFAULTS to at boot (the OSD can switch modes at
# runtime via config + reboot): 240P (stock 126 MHz), 480P (252 MHz / VREG
# 1.15V, runs from RAM) or 720P (372 MHz / VREG 1.30V, runs from RAM).
set(N64_HDMI_MODE "240P" CACHE STRING "Default HDMI output mode: 240P, 480P or 720P")
set_property(CACHE N64_HDMI_MODE PROPERTY STRINGS 240P 480P 720P)

# HSTX TMDS pinout preset: which of the RP2350's GPIO12-19 carry the
# CLK/D0/D1/D2 differential pairs. 'dvisock' (default) is the Pimoroni
# Pico DVI Sock (GP12=D0+, GP13=D0-, GP14=CK+, GP15=CK-, GP16=D2+,
# GP17=D2-, GP18=D1+, GP19=D1-). 'pico_hdmi' is the library's own default
# wiring used by the PicoDVI-N64-style boards (CLK 12/13, D0 14/15,
# D1 16/17, D2 18/19).
set(N64_HDMI_PINOUT "dvisock" CACHE STRING "HSTX TMDS pinout preset: dvisock (default) or pico_hdmi")
set_property(CACHE N64_HDMI_PINOUT PROPERTY STRINGS dvisock pico_hdmi)

set(CONFIG_DEFAULT_SAMPLE_RATE_HZ "SAMPLE_RATE_48000_HZ" CACHE STRING
    "Default HDMI audio sample rate (SAMPLE_RATE_{32000,44100,48000,96000}_HZ)")
set(CONFIG_DEFAULT_COLOR_DEPTH "DVI_RGB_565" CACHE STRING
    "Default color depth (DVI_RGB_555 or DVI_RGB_565)")
# Set to OFF to build without the audio DMA/background-task machinery at
# all - useful to isolate whether audio (which runs continuously on Core 1
# via video_output_set_background_task(), competing with HSTX's own
# scanline-service timing) is contributing to any picture instability,
# the same class of problem __scratch_x/hot-loop-overhead bugs turned out
# to be earlier in this project.
set(N64_HDMI_AUDIO_ENABLED ON CACHE BOOL "Build with HDMI audio (N64 audio -> Data Islands)")
# Audio machinery ON but pushing silence packets: keeps the DI queue, ACR,
# infoframes and background-task islands exactly as the real-audio build,
# but the task feeds pre-encoded silence and NO N64 audio capture hardware
# runs (no audio PIO SM, no DMA/timer ring). Isolation test for "480p +
# audio = no signal": if this shows a picture, the fault is in the audio
# capture path; if it still shows no signal, the island machinery itself
# breaks the 480p output.
set(N64_HDMI_AUDIO_SILENT OFF CACHE BOOL "Audio: push silence islands, no capture hardware (isolation build)")
# With N64_HDMI_AUDIO_SILENT=ON, additionally keep the audio PIO SM1 enabled
# on PIO1 (no DMA): isolates whether SM1's mere presence (PIO1 round-robin
# slots shared with the video capture SM0) breaks the picture.
set(N64_HDMI_AUDIO_SILENT_SM1 OFF CACHE BOOL "Silent build: also enable audio PIO SM1 (PIO1 sharing test)")
# Audio-only test build: no N64 video capture at all (no video SM0 on PIO1,
# Core 0 idles), scanline callback draws a static bar pattern, and the full
# real audio capture (PIO SM1 + DMA/timer + background task) runs. Answers
# "does the N64 audio capture -> HDMI islands path produce sound at all?"
set(N64_HDMI_AUDIO_ONLY OFF CACHE BOOL "Audio-only: no video capture, bar pattern output")
# 240p (output_mode 0) but with sys_clk at 252 MHz: keeps the 25.2 MHz 240p
# wire signal (n64_hdmi_240p_confirmed's hstx_clk_div=2) while giving the
# N64 capture loop enough CPU headroom. Stock 126 MHz 240p loses PIO FIFO
# headroom mid-line and shows up as garbage-with-the-game's-colors. On by
# default: hardware testing confirmed the 126 MHz 240p raster is genuinely
# broken (garbage-with-game's-colors on a real N64), and the 252 MHz signal
# is byte-identical on the wire (same 25.2 MHz pixel clock).
set(N64_HDMI_240P_OC252 ON CACHE BOOL "240p mode: run sys_clk at 252 MHz (capture headroom fix)" FORCE)

option(N64_HDMI_OSD_FORCE_OPEN "Diagnostic: open the OSD overlay once at boot" OFF)

# Use the runtime-mode-switching variant of pico_hdmi (video_output_rt.c),
# with runtime mode attributes so the 720p CEA descriptor's positive sync
# polarity and back-porch Data Island placement are honored (the top-level
# shared build keeps this OFF for 480p/240p).
#
# N64_HDMI_720P_BUILD: when OFF, compile the library exactly like the
# pre-720p 480p/240p builds (no runtime mode attrs, no VIDEO_MODE_1280x720,
# no active-line double buffer). Isolation control to bisect the 480P
# rolling-band regression between build-osd-480p-audio (works) and
# build-osd-3modes (fails).
set(N64_HDMI_720P_BUILD ON CACHE BOOL "Build with 720p support (runtime mode attrs + double buffer)")
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)
if(N64_HDMI_720P_BUILD)
    set(PICO_HDMI_RT_RUNTIME_MODE_ATTRS ON CACHE BOOL "" FORCE)

    # Give 720p the firmware scanline callback a pipelined 3-line-period budget
    # (PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER engages only when v_active_lines==720).
    # Hardware testing: the general zoom/pan/blur transform at 1280px cost ~10-13k
    # cycles/line vs the ~6.4k-cycle one-segment ISR budget at 372 MHz, the HSTX
    # FIFO underflowed, and the sink dropped the signal whenever Zoom/Pos/Blur
    # were changed at 720P. 240P/480P are unaffected (option is inert there).
    set(PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../lib/pico_hdmi pico_hdmi)

# Shared assets live in software/assets (same layout as upstream PicoDVI-N64):
# font_8x8.h is picked up from there, not from the app folder.
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../assets)

add_executable(n64_hdmi_osd_overlay
    config.c
    gfx.c
    joybus.c
    main.c
    osd.c
)

# -Wall on the app sources only: the vendored pico_hdmi library has a
# pre-existing unused-function warning (htrim_register_all with runtime mode
# attrs) that the reference builds don't surface because they compile the
# library without -Wall.
target_compile_options(n64_hdmi_osd_overlay PRIVATE -Wall)

# VIDEO_MODE_1280x720 MUST be defined on BOTH the library target and the
# executable target - DI_HSYNC_ACTIVE resolves at compile time in each
# translation unit, and a mismatch corrupts the embedded HSYNC bits in
# audio Data Island packets. (Upstream's own comment, preserved verbatim
# because it's a real correctness requirement, not boilerplate.)
if(N64_HDMI_720P_BUILD)
    target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE VIDEO_MODE_1280x720)
endif()

# The 480p and 720p modes overclock sys_clk to 252/372 MHz. At those speeds
# the QMI flash read timing (set by boot stage2 for the boot clock) no longer
# samples correctly, so XIP instruction fetches corrupt and the core
# hard-faults. Execute from RAM always (harmless at 126 MHz).
pico_set_binary_type(n64_hdmi_osd_overlay copy_to_ram)

target_compile_definitions(n64_hdmi_osd_overlay PRIVATE
    CONFIG_DEFAULT_SAMPLE_RATE_HZ=${CONFIG_DEFAULT_SAMPLE_RATE_HZ}
    CONFIG_DEFAULT_COLOR_DEPTH=${CONFIG_DEFAULT_COLOR_DEPTH}
    # The OSD menu paints over the LIVE capture (the scanline callback blits
    # the pre-rendered menu box over each line) instead of the menu freezing
    # the captured frame.
    N64_HDMI_OSD_OVERLAY
)

# HSTX pinout preset: only 'dvisock' needs a library override call in
# main.c (main() calls video_output_set_hstx_pinout() when
# N64_HDMI_PINOUT_DVI_SOCK is defined); 'pico_hdmi' is the library default.
if(N64_HDMI_PINOUT STREQUAL "dvisock")
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_PINOUT_DVI_SOCK)
elseif(NOT N64_HDMI_PINOUT STREQUAL "pico_hdmi")
    message(FATAL_ERROR "N64_HDMI_PINOUT must be 'dvisock' or 'pico_hdmi'")
endif()

if(N64_HDMI_OSD_FORCE_OPEN)
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_OSD_FORCE_OPEN)
endif()

if(NOT N64_HDMI_AUDIO_ENABLED)
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE AUDIO_ENABLED=0)
endif()

if(N64_HDMI_AUDIO_SILENT)
    if(NOT N64_HDMI_AUDIO_ENABLED)
        message(FATAL_ERROR "N64_HDMI_AUDIO_SILENT requires N64_HDMI_AUDIO_ENABLED=ON")
    endif()
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_AUDIO_SILENT)
endif()

if(N64_HDMI_AUDIO_SILENT_SM1)
    if(NOT N64_HDMI_AUDIO_SILENT)
        message(FATAL_ERROR "N64_HDMI_AUDIO_SILENT_SM1 requires N64_HDMI_AUDIO_SILENT=ON")
    endif()
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_AUDIO_SILENT_SM1)
endif()

if(N64_HDMI_AUDIO_ONLY)
    if(NOT N64_HDMI_AUDIO_ENABLED)
        message(FATAL_ERROR "N64_HDMI_AUDIO_ONLY requires N64_HDMI_AUDIO_ENABLED=ON")
    endif()
    if(N64_HDMI_AUDIO_SILENT)
        message(FATAL_ERROR "N64_HDMI_AUDIO_ONLY and N64_HDMI_AUDIO_SILENT are mutually exclusive")
    endif()
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_AUDIO_ONLY)
endif()

# The default boot mode: only picks config_init()'s default output_mode
# (0 = 240P, 1 = 480P, 2 = 720P). The OSD's "Video Mode" + "Save & Reboot"
# can switch modes at runtime in any build; main() reads the saved
# output_mode at boot and picks the matching sys_clk / mode descriptor.
if(N64_HDMI_MODE STREQUAL "480P")
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_OUTPUT_640x480)
elseif(N64_HDMI_MODE STREQUAL "720P")
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_OUTPUT_1280x720)
elseif(NOT N64_HDMI_MODE STREQUAL "240P")
    message(FATAL_ERROR "N64_HDMI_MODE must be 240P, 480P or 720P")
endif()
# (240P needs no define: it's config.c's #else branch, default output_mode 0.)

if(N64_HDMI_240P_OC252)
    target_compile_definitions(n64_hdmi_osd_overlay PRIVATE N64_HDMI_240P_OC252)
endif()

target_link_libraries(n64_hdmi_osd_overlay
    pico_stdlib
    pico_multicore
    pico_util
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_irq
    hardware_vreg
    hardware_flash
    hardware_watchdog
    pico_hdmi
)

# Build the capture PIO programs (unchanged from PicoDVI-N64)
pico_generate_pio_header(n64_hdmi_osd_overlay ${CMAKE_CURRENT_LIST_DIR}/joybus.pio)
pico_generate_pio_header(n64_hdmi_osd_overlay ${CMAKE_CURRENT_LIST_DIR}/n64.pio)

pico_enable_stdio_uart(n64_hdmi_osd_overlay 1)
pico_enable_stdio_usb(n64_hdmi_osd_overlay 0)

pico_add_extra_outputs(n64_hdmi_osd_overlay)
