/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann
 */

#pragma once

#include "joybus.h"
#include "gfx.h"
#include <stdbool.h>

#ifdef N64_HDMI_OSD_OVERLAY
// ----------------------------------------------------------------------------
// OSD overlay mode: the menu is pre-rendered into a 1-bit-per-pixel bitmap
// (one row per source line the menu covers), and the scanline callback paints
// it over the LIVE capture, which keeps running underneath. The bitmap is 0
// = background (black, or magenta on the focused item) and 1 = foreground
// text; the per-item colors and text rectangle are stored alongside so the
// callback only has to paint the characters, not the whole row.
// ----------------------------------------------------------------------------
#define OSD_MENU_TOP_ROW (OSD_Y_OFFSET * 8)          // first source row the menu covers
#define OSD_ITEMS_MAX 12
#define OSD_MENU_ROW_SPAN (OSD_ITEMS_MAX * 8)        // max menu height in source rows
#define OSD_BITMAP_BYTES_PER_ROW (FRAME_WIDTH / 8)

extern uint8_t  g_osd_bitmap[OSD_MENU_ROW_SPAN][OSD_BITMAP_BYTES_PER_ROW];
extern uint16_t g_osd_fg[OSD_ITEMS_MAX];             // text color per menu item
extern uint16_t g_osd_bg[OSD_ITEMS_MAX];             // background color per item
extern uint16_t g_osd_x0[OSD_ITEMS_MAX];             // text left edge in source pixels
extern uint16_t g_osd_w[OSD_ITEMS_MAX];              // text width in source pixels
extern uint32_t g_osd_span_rows;                     // current menu height (multiple of 8)
extern uint16_t g_osd_box_x0;                        // solid menu box left edge in source pixels
extern uint16_t g_osd_box_w;                         // solid menu box width (widest item)
// Pre-rendered menu box rows (480P layout: one 32-bit word = two pixels), so
// the scanline paint is a single memcpy instead of per-word box fill + text.
// Built once per osd_render(); the callback blits it over the box rectangle.
#define OSD_BOX_WORDS_MAX 160                        // max box width in 480P words (2 px each)
extern uint32_t g_osd_box_buf[OSD_MENU_ROW_SPAN][OSD_BOX_WORDS_MAX];

bool osd_is_open(void);
#endif

/**
 * @brief Key combination to enter the OSD menu.
 *
 * This macro checks if the specified key combination is pressed.
 * The key combination for the OSD menu is L + R + C-DOWN + DPAD-DOWN.
 *
 * @param __keys__ The current state of the keys.
 * @return True if the OSD shortcut keys are pressed, false otherwise.
 */
#define OSD_SHORTCUT(__keys__) ( \
    TL_BUTTON(__keys__) && \
    TR_BUTTON(__keys__) && \
    CD_BUTTON(__keys__) && \
    DD_BUTTON(__keys__)    \
)

/**
 * @brief Run the OSD.
 *
 * This function runs the OSD. It should be called in your main loop.
 * In overlay mode it is non-blocking: one input event / redraw per call, so
 * the N64 capture loop keeps running while the menu is open.
 */
void osd_run(void);
