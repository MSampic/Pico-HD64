/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann
 * Copyright (c) 2026 - HSTX/pico_hdmi port
 */

/**
 * @file config.h
 * @brief Configuration management functions and global configuration variable.
 */

#pragma once

#include <stdint.h>

/// Default crop X parameter for PAL.
#define DEFAULT_CROP_X_PAL  (36)

/// Default crop X parameter for NTSC.
#define DEFAULT_CROP_X_NTSC (14)

/// Default crop Y parameter for PAL.
#define DEFAULT_CROP_Y_PAL  (90)

/// Default crop Y parameter for NTSC.
#define DEFAULT_CROP_Y_NTSC (25)

/// Number of rows for PAL.
#define ROWS_PAL            (615)

/// Number of rows for NTSC.
#define ROWS_NTSC           (511)

/// Tolerance for number of rows.
#define ROWS_TOLERANCE      (5)

/// Y offset for OSD.
#define OSD_Y_OFFSET (3)

/// Vertical pan (source rows) added to the GAME viewport ONLY while the menu
/// is open, via the Pos Y mechanism. Opening the menu makes the game appear to
/// jump a few rows (the capture and HDMI output share one framebuffer and are
/// not phase-locked); panning the game down while open puts it back. The OSD
/// box band is NOT shifted and the "Pos Y" item keeps showing the raw config
/// value. 0 disables it.
#define OSD_OPEN_Y_SHIFT (7)        // 240P / 480P (1 source row = 1px / 2px)
#define OSD_OPEN_Y_SHIFT_720P (4)   // 720P (1 source row = 3px)

/// X offset for OSD.
#define OSD_X_OFFSET (5)

/**
 * pico_hdmi (HSTX) output notes
 * ------------------------------
 * The old libdvi backend bit-banged TMDS through PIO and needed an
 * overclocked 252 MHz / 1.2V system clock just to keep up. HSTX is a
 * dedicated hardware TMDS encoder on RP2350, so both supported output
 * modes below run at the *stock* 126 MHz system clock (25.2 MHz pixel
 * clock) with default core voltage - see main.c. No vreg_set_voltage()
 * call is required for either mode.
 *
 * N64_HDMI_OUTPUT_320x240 (default, set via CMake -DN64_HDMI_MODE=240P):
 *   True 240p from the N64's native 640-sample-wide capture (2x horizontal
 *   repetition -> 1280x240 output) - the whole point of this project.
 *   pico_hdmi repeats each pixel horizontally to keep a spec-legal
 *   25.2 MHz pixel clock while emitting a genuine low-res 240p signal,
 *   which is exactly what capture cards / RetroTINK / OSSC-style scalers
 *   expect from a retro console.
 *
 * N64_HDMI_OUTPUT_640x480 (set via CMake -DN64_HDMI_MODE=480P):
 *   2x2 upscale to 640x480 for plugging directly into a normal HDMI
 *   display/TV without an external scaler.
 *
 * N64_HDMI_OUTPUT_1280x720 (set via CMake -DN64_HDMI_MODE=720P):
 *   720p60 CEA output (372 MHz sys_clk / VREG 1.30V, executes from RAM):
 *   2x horizontal + 3x vertical scale of the 640x240 capture. A 4:3 option
 *   in the OSD narrows the picture to a centered 960px region with side bars
 *   (this build is standalone - 720p CEA needs PICO_HDMI_RT_RUNTIME_MODE_ATTRS
 *   on its own copy of the pico_hdmi library, see apps/n64_hdmi_osd_overlay/CMakeLists.txt).
 *
 * Regardless of which mode the build defaults to, all three modes (240P /
 * 480P / 720P) are available at runtime: the OSD's "Video Mode" item stages
 * output_mode and "Save & Reboot" persists it, and main() picks the matching
 * sys_clk / descriptor at boot.
 */

/// UART config on the last GPIOs
/// UART transmission pin.
#define UART_TX_PIN (28)

/// UART reception pin (not available on the pico).
#define UART_RX_PIN (29)

/// UART identifier.
#define UART_ID     uart0

/// Baud rate for UART communication.
#define BAUD_RATE   115200

/**
 * @brief The number of bits to shift to get the audio buffer size.
 *
 * This value is used to calculate the size of the audio buffer by shifting
 * 1 to the left by this number of bits. For example, if
 * AUDIO_BUFFER_SIZE_BITS is 12, then the audio buffer size will be
 * 1 << 12, or 4096.
 */
#define AUDIO_BUFFER_SIZE_BITS 12

/**
 * @brief The size of the audio buffer.
 *
 * This is calculated as 1 << AUDIO_BUFFER_SIZE_BITS. So if
 * AUDIO_BUFFER_SIZE_BITS is 12, then AUDIO_BUFFER_SIZE will be 4096.
 */
#define AUDIO_BUFFER_SIZE (1 << AUDIO_BUFFER_SIZE_BITS)

/**
 * @def AUDIO_ENABLED
 * @brief A macro to control the audio input functionality.
 *
 * When set to 1, the audio input functionality is enabled. This means that the
 * application will process and output audio data over HDMI Data Islands.
 * This only works if the audio input wires (LRCLK/SDAT/BCLK) have been
 * correctly soldered to the appropriate pins on the hardware.
 *
 * When set to 0, the audio input functionality is disabled. This can be useful
 * for debugging, or when running on hardware where the audio input wires have
 * not been soldered - the stream is then emitted as plain DVI (no Data Islands).
 */
#ifndef AUDIO_ENABLED
#define AUDIO_ENABLED 1
#endif

/**
 * @def DIAGNOSTICS
 * @brief A macro to control the display of diagnostic data.
 *
 * When defined, the application will print diagnostic data on the screen.
 * This can be useful for debugging and performance tuning.
 *
 * When not defined, no diagnostic data will be displayed.
 */
// #define DIAGNOSTICS

/**
 * @def DIAGNOSTICS_JOYBUS
 * @brief A macro to control the display of Joybus diagnostic data.
 *
 * When defined, the application will print Joybus-specific diagnostic data
 * on the screen. This can be useful for debugging and performance tuning
 * of the Joybus communication.
 *
 * When not defined, no Joybus diagnostic data will be displayed.
 */
// #define DIAGNOSTICS_JOYBUS

/**
 * @def CONFIG_MAGIC1
 * @brief A magic number used for configuration validation.
 */
#define CONFIG_MAGIC1 0x12345678

/**
 * @def CONFIG_MAGIC2
 * @brief A second magic number used for configuration validation.
 */
#define CONFIG_MAGIC2 0xdeadf00d

/**
 * @def CONFIG_GEN
 * @brief Generation of the config layout, stored in config_t.
 *
 * Bump this whenever the config layout/semantics change: config_load()
 * ignores any saved config whose generation differs, falling back to
 * defaults. This is the self-healing guard that lets a plain re-flash
 * recover from a bad saved config (the config sector lives in the last
 * flash sector, which normal UF2 flashing never touches).
 */
#define CONFIG_GEN 2u

/**
 * @enum dvi_color_mode
 * @brief Enumerates the supported color modes.
 *
 * pico_hdmi's scanline callback always consumes RGB565 words, so RGB555
 * is implemented as RGB565 with the low green bit forced to zero (same
 * behavior as the original code) rather than as a distinct wire format.
 */
typedef enum dvi_color_mode {
    DVI_RGB_555 = 0, ///< 15-bit color mode (5 bits each for red, green, and blue).
    DVI_RGB_565,     ///< 16-bit color mode (5 bits for red and blue, 6 bits for green).
    DVI_RGB_888,     ///< 24-bit color mode (8 bits each for red, green, and blue). Unused by this backend.
} dvi_color_mode_t;

/**
 * @enum sample_rate_hz
 * @brief Enumerates the supported output audio sample rates in Hertz.
 */
typedef enum sample_rate_hz {
    SAMPLE_RATE_32000_HZ = 32000, ///< 32,000 Hz sample rate.
    SAMPLE_RATE_44100_HZ = 44100, ///< 44,100 Hz sample rate.
    SAMPLE_RATE_48000_HZ = 48000, ///< 48,000 Hz sample rate.
    SAMPLE_RATE_96000_HZ = 96000, ///< 96,000 Hz sample rate.
} sample_rate_hz_t;

/**
 * @enum video_region
 * @brief Enumerates the forced capture crop regions.
 *
 * 0 (Auto) keeps the frame-rate auto-detection (ROWS_PAL/ROWS_NTSC); the
 * others pin the crop to one region so a misdetected (or deliberately
 * mislabelled) game is not cropped on top/bottom. PAL is 576 active lines
 * vs 480 for NTSC, so the PAL crop (DEFAULT_CROP_Y_PAL) trims more from the
 * top and the 240-line capture truncates more from the bottom.
 */
typedef enum video_region {
    VIDEO_REGION_AUTO  = 0, ///< Auto-detect from the captured frame rate.
    VIDEO_REGION_NTSC  = 1, ///< Force NTSC crop values.
    VIDEO_REGION_PAL   = 2, ///< Force PAL crop values.
} video_region_t;

/**
 * @struct config
 * @brief Represents the configuration for the application.
 */
typedef struct config {
    uint32_t magic1;                ///< The first magic number used for configuration validation.
    uint32_t audio_out_sample_rate; ///< The audio output sample rate in Hertz (see @ref sample_rate_hz_t).
    uint32_t dvi_color_mode;        ///< The color mode (see @ref dvi_color_mode_t).

    // OSD-adjustable render settings.
    uint32_t output_mode;           ///< 0 = 240P, 1 = 480P, 2 = 720P (staged in the OSD; applied by "Save & Reboot" at boot, which also picks the sys_clk).
    uint32_t zoom_percent;          ///< 100..400, percent of the 640x240 capture shown (100 = full frame; zooming in expands from the center).
    int32_t  pos_x;                 ///< Viewport offset from the frame CENTER, source pixels (signed; may push off-edge -> black fill, so panning works even at zoom=100).
    int32_t  pos_y;                 ///< Viewport offset from the frame CENTER, source pixels (signed; black fill off-edge).
    uint32_t blur_enabled;          ///< 0/1 software blur/antialiasing: softens the upscaled axis (vertical in 480P, horizontal in 240P) and horizontally while zoomed.
    uint32_t aspect_ratio;          ///< 0 = 16:9 (fill the full output width), 1 = 4:3 (centered 960px picture with side bars at 720P). Only has an effect at 720P; 480P/240P ignore it.

    uint32_t config_gen;            ///< Layout generation (CONFIG_GEN). Mismatch on load = ignore the saved config.
    uint32_t magic2;                ///< The second magic number used for configuration validation.

    ///< Latch set by the boot-combo recovery (C-Up + one of C-Left/Down/Right
    ///< at power-on picks 240P/480P/720P): the combo fires once, saves this
    ///< flag, then watchdog-reboots; the recovery boot sees it and disarms the
    ///< combo so it does not re-fire while the buttons are still held (the N64
    ///< keeps running and polling through the Pico's reboot, which would
    ///< otherwise loop forever). It is cleared on the first non-combo poll
    ///< once the user lets go. Appended after magic2 so older saved configs
    ///< still validate (their tail reads as erased flash 0xFF, normalized to 0
    ///< on load).
    uint32_t boot_combo_fired;      ///< 0/1: a boot combo already fired.

    ///< 0 = Auto, 1 = NTSC, 2 = PAL (see @ref video_region_t): which capture
    ///< crop is applied to the capture loop. Appended after boot_combo_fired
    ///< so older saved configs still validate (their tail reads as erased
    ///< flash 0xFF, normalized to 0 on load).
    uint32_t video_region;          ///< Forced capture region (see @ref video_region_t).
} config_t;

/**
 * @brief Global configuration variable.
 */
extern config_t g_config;

/**
 * @brief Initializes the global configuration with default values.
 */
void config_init(void);

/**
 * @brief Loads the configuration from flash memory into g_config.
 */
void config_load(void);

/**
 * @brief Saves the configuration from g_config to flash memory.
 */
void config_save(void);

/**
 * @brief Erases the config sector, so the next boot uses defaults.
 *
 * Used by the BOOTSEL "factory reset" path: it only clears the saved
 * config; it does not touch the application image.
 */
void config_erase(void);
