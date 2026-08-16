/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann
 */

#include "config.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"

config_t g_config;

// Config storage: the last flash sector (4 KB). The bootloader/app image
// sits at the start of flash, so the tail sector is always free; the board
// defaults to 4 MB (pico2), giving a sector at offset 0x3FF000. A single
// 256-byte page holds the whole config_t (well under 256 bytes).
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_STORAGE_BYTES (FLASH_PAGE_SIZE)

// flash_range_program() wants a page-aligned RAM source buffer.
static uint8_t config_storage[CONFIG_STORAGE_BYTES] __attribute__((aligned(FLASH_PAGE_SIZE)));

// Allow for compile-time configuration of default sample rate
#ifndef CONFIG_DEFAULT_SAMPLE_RATE_HZ
#define CONFIG_DEFAULT_SAMPLE_RATE_HZ SAMPLE_RATE_96000_HZ
#endif

// Allow for compile-time configuration of default color depth
#ifndef CONFIG_DEFAULT_COLOR_DEPTH
#define CONFIG_DEFAULT_COLOR_DEPTH DVI_RGB_555
#endif

static config_t default_config = {
    .magic1 = CONFIG_MAGIC1,

    .audio_out_sample_rate = CONFIG_DEFAULT_SAMPLE_RATE_HZ,
    .dvi_color_mode = CONFIG_DEFAULT_COLOR_DEPTH,

#ifdef N64_HDMI_OUTPUT_1280x720
    .output_mode = 2, // 720P
#elif defined(N64_HDMI_OUTPUT_640x480)
    .output_mode = 1, // 480P
#else
    .output_mode = 0, // 240P
#endif
    .zoom_percent = 100,
    .pos_x = 0,
    .pos_y = 0,
    .blur_enabled = 0,
    .aspect_ratio = 0, // 16:9
    .config_gen = CONFIG_GEN,

    .magic2 = CONFIG_MAGIC2,

    .video_region = 0, // AUTO
};

void config_init(void)
{
    memcpy(&g_config, &default_config, sizeof(config_t));
}

void config_load(void)
{
    // Read directly via XIP (safe: we're running from RAM, copy_to_ram).
    const uint8_t *stored = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    config_t tmp;
    memcpy(&tmp, stored, sizeof(config_t));
    if (tmp.magic1 == CONFIG_MAGIC1 && tmp.magic2 == CONFIG_MAGIC2 &&
        tmp.config_gen == CONFIG_GEN) {
        memcpy(&g_config, &tmp, sizeof(config_t));
        // Normalize fields (older/foreign saves may read junk).
        g_config.output_mode = (g_config.output_mode <= 2) ? g_config.output_mode : 0;
        g_config.blur_enabled = g_config.blur_enabled == 1 ? 1 : 0;
        g_config.aspect_ratio = g_config.aspect_ratio == 1 ? 1 : 0;
        g_config.boot_combo_fired = g_config.boot_combo_fired == 1 ? 1 : 0;
        g_config.video_region = (g_config.video_region <= 2) ? g_config.video_region : 0;
        if (g_config.zoom_percent < 50 || g_config.zoom_percent > 400) {
            g_config.zoom_percent = 100;
        }
    }
    // Any mismatch (magic or generation) keeps the compile-time defaults:
    // a saved config written by an older firmware is ignored, which is how
    // a plain re-flash self-heals a bad boot mode (the config sector is
    // never touched by normal UF2 flashing).
}

void config_erase(void)
{
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

void config_save(void)
{
    memset(config_storage, 0xff, sizeof(config_storage));
    memcpy(config_storage, &g_config, sizeof(config_t));
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, config_storage, CONFIG_STORAGE_BYTES);
}
