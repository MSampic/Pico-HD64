/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann
 */

#include "config.h"

#include <string.h>
#include <stdio.h>

#include "gfx.h"
#include "osd.h"
#include "joybus.h"
#include "app.h"

#include "pico_hdmi/video_output_rt.h"

#include "font_8x8.h"

#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32

typedef enum item_type {
    ITEM_TYPE_TEXT = 0,
    ITEM_TYPE_VALUE_RW_U32,
    ITEM_TYPE_VALUE_RW_I32,
    ITEM_TYPE_VALUE_RO_U32,
    ITEM_TYPE_VALUE_RO_I32,
    ITEM_TYPE_MENU,
    ITEM_TYPE_BACK,
    ITEM_TYPE_EXIT,
    ITEM_TYPE_ACTION,
} item_type_t;

typedef struct menu_item {
    char *text;
    item_type_t type;
    union {
        uint32_t *value_u32;
        int32_t *value_i32;
        void *value_ptr;
    } value;
    int step;       // RW items: increment/decrement per D-pad left/right
    int min;        // RW items: clamp range (inclusive)
    int max;
    void (*on_change)(void); // called after a RW item changes
} menu_item_t;

menu_item_t menu[] = {
    {
        .text = "OSD Menu",
    },
    {
        .text = "Video Mode",
        .type = ITEM_TYPE_VALUE_RW_U32,
        .value.value_u32 = &g_config.output_mode,
        .step = 1,
        .min = 0,
        .max = 2,
        // 0 = 240P, 1 = 480P, 2 = 720P. The mid-stream live switch drops sync
        // on some monitors and never recovers, so a mode change is staged here
        // and only applied by "Save & Reboot" below (persist to flash +
        // reboot, letting boot-time init pick it up, including the matching
        // sys_clk). No on_change: the running output is never disturbed by
        // editing the value.
    },
    {
        .text = "Zoom",
        .type = ITEM_TYPE_VALUE_RW_U32,
        .value.value_u32 = &g_config.zoom_percent,
        .step = 10,
        .min = 50,
        .max = 400,
    },
    {
        .text = "Pos X",
        .type = ITEM_TYPE_VALUE_RW_I32,
        .value.value_i32 = &g_config.pos_x,
        .step = 1,
        .min = -300,
        .max = 300,
    },
    {
        .text = "Pos Y",
        .type = ITEM_TYPE_VALUE_RW_I32,
        .value.value_i32 = &g_config.pos_y,
        .step = 1,
        .min = -110,
        .max = 110,
    },
    {
        .text = "Blur",
        .type = ITEM_TYPE_VALUE_RW_U32,
        .value.value_u32 = &g_config.blur_enabled,
        .step = 1,
        .min = 0,
        .max = 1,
    },
    {
        .text = "Aspect",
        .type = ITEM_TYPE_VALUE_RW_U32,
        .value.value_u32 = &g_config.aspect_ratio,
        .step = 1,
        .min = 0,
        .max = 1,
        // 0 = 16:9 (fill the output width), 1 = 4:3 (centered 960px picture
        // with side bars at 720P). Only has an effect while the output mode
        // is 720P; the item is hidden otherwise (see osd_item_visible()).
    },
    {
        .text = "Video Region",
        .type = ITEM_TYPE_VALUE_RW_U32,
        .value.value_u32 = &g_config.video_region,
        .step = 1,
        .min = 0,
        .max = 2,
        // 0 = Auto (detect PAL/NTSC from the captured frame rate), 1 = NTSC,
        // 2 = PAL. Forces the capture-loop crop, so a game whose region is
        // misdetected (or which runs an exotic mode) stops getting cropped
        // top/bottom. Applies live - no reboot needed.
    },
    {
        .text = "Reset N64",
        .type = ITEM_TYPE_ACTION,
        .on_change = app_reset_n64,
    },
    {
        .text = "Save & Reboot",
        .type = ITEM_TYPE_ACTION,
        .on_change = app_save_config_and_reboot,
    },
    {
        .text = "Exit OSD",
        .type = ITEM_TYPE_EXIT,
    },
    {
        .text = NULL,
    }
};

static struct {
    uint32_t last_buttons;
    menu_item_t *root;
    menu_item_t *current_root;
    menu_item_t *focused_item;
    menu_item_t *stack[8];
    bool open;
} state = {
    .root = menu,
    .stack = {menu},
};

#define BUTTON_PRESSED(__op__) (!__op__(state.last_buttons) && __op__(buttons))

// Menu items can be hidden based on the active output mode: the "Aspect"
// item only makes sense at 720P (4:3/16:9 has no effect in 480P/240P), so it
// is skipped in rendering and navigation while the output isn't 720P. The
// title ("OSD Menu") is always visible, which also bounds the up/down
// navigation loops below.
static bool osd_item_visible(const menu_item_t *item)
{
    if (item->value.value_u32 == &g_config.aspect_ratio) {
        return video_output_active_mode->v_active_lines == 720U;
    }
    return true;
}

#ifdef N64_HDMI_OSD_OVERLAY
// ----------------------------------------------------------------------------
// OSD overlay mode. The menu is pre-rendered into a 1-bit bitmap (see
// osd.h): one row of FRAME_WIDTH bits per source line the menu covers,
// starting at source row OSD_MENU_TOP_ROW. 1 = text, 0 = background. The
// scanline callback paints only the text rectangle per item, so the live
// N64 capture (which keeps running while the menu is open) stays visible
// around the menu.
// ----------------------------------------------------------------------------
uint8_t  g_osd_bitmap[OSD_MENU_ROW_SPAN][OSD_BITMAP_BYTES_PER_ROW];
uint16_t g_osd_fg[OSD_ITEMS_MAX];
uint16_t g_osd_bg[OSD_ITEMS_MAX];
uint16_t g_osd_x0[OSD_ITEMS_MAX];
uint16_t g_osd_w[OSD_ITEMS_MAX];
uint32_t g_osd_span_rows = 0;
uint16_t g_osd_box_x0 = 0;
uint16_t g_osd_box_w = 0;
uint32_t g_osd_box_buf[OSD_MENU_ROW_SPAN][OSD_BOX_WORDS_MAX];

bool osd_is_open(void)
{
    return state.open;
}

#ifdef N64_HDMI_OSD_FORCE_OPEN
// Diagnostic: open the menu once at boot (no chord needed) so the overlay
// paint can be verified against a live capture without touching the
// controller. Close it again with A on "Exit OSD".
static bool osd_force_open_once(void)
{
    static bool done = false;
    if (done) {
        return false;
    }
    done = true;
    return true;
}
#endif

static void osd_render_item(uint32_t item_idx, const char *text)
{
    if (item_idx >= OSD_ITEMS_MAX) {
        return;
    }
    // Center each item horizontally in the source frame so the menu is
    // independent of the N64 picture's horizontal pan/zoom offset.
    const uint32_t text_w = strlen(text) * FONT_CHAR_WIDTH;
    const uint32_t xbase = (FRAME_WIDTH - text_w) / 2;
    g_osd_x0[item_idx] = xbase;
    g_osd_w[item_idx] = text_w;
    for (int py = 0; py < FONT_CHAR_HEIGHT; py++) {
        uint8_t *bits = g_osd_bitmap[item_idx * FONT_CHAR_HEIGHT + py];
        memset(bits, 0, OSD_BITMAP_BYTES_PER_ROW);
        const char *ptr = text;
        uint32_t x = xbase;
        char c;
        while ((c = *ptr++)) {
            uint8_t fbits = font_8x8[(c - FONT_FIRST_ASCII) + py * FONT_N_CHARS];
            for (int i = 0; i < FONT_CHAR_WIDTH; i++) {
                if (fbits & (1u << i)) {
                    uint32_t bx = x + i;
                    bits[bx >> 3] |= (uint8_t)(0x80u >> (bx & 7));
                }
            }
            x += FONT_CHAR_WIDTH;
        }
    }
}

static void osd_render(void)
{
    menu_item_t *item = state.current_root;
    uint32_t idx = 0;
    while (item && item->text && idx < OSD_ITEMS_MAX) {
        if (!osd_item_visible(item)) {
            item++;
            continue;
        }
        uint16_t bg = (item == state.focused_item)
                          ? RGB888_TO_RGB565(0xff, 0x00, 0xff)
                          : RGB888_TO_RGB565(0x00, 0x00, 0x00);
        uint16_t fg = RGB888_TO_RGB565(0xff, 0xff, 0xff);
        g_osd_bg[idx] = bg;
        g_osd_fg[idx] = fg;

        char text[64];
        if (item->type == ITEM_TYPE_VALUE_RW_U32) {
            if (item->value.value_u32 == &g_config.output_mode) {
                const char *m = *item->value.value_u32 == 2 ? "720P"
                              : *item->value.value_u32 == 1 ? "480P"
                                                            : "240P";
                snprintf(text, sizeof(text), "%s: %s", item->text, m);
            } else if (item->value.value_u32 == &g_config.aspect_ratio) {
                snprintf(text, sizeof(text), "%s: %s",
                    item->text, *item->value.value_u32 ? "4:3" : "16:9");
            } else if (item->value.value_u32 == &g_config.video_region) {
                const char *r = *item->value.value_u32 == 2 ? "PAL"
                              : *item->value.value_u32 == 1 ? "NTSC"
                                                            : "Auto";
                snprintf(text, sizeof(text), "%s: %s", item->text, r);
            } else if (item->value.value_u32 == &g_config.blur_enabled) {
                snprintf(text, sizeof(text), "%s: %s",
                    item->text, *item->value.value_u32 ? "ON" : "OFF");
            } else {
                snprintf(text, sizeof(text), "%s: %lu",
                    item->text, (unsigned long)*item->value.value_u32);
            }
        } else if (item->type == ITEM_TYPE_VALUE_RW_I32) {
            snprintf(text, sizeof(text), "%s: %ld",
                item->text, (long)*item->value.value_i32);
        } else {
            snprintf(text, sizeof(text), "%s", item->text);
        }

        osd_render_item(idx, text);
        idx++;
        item++;
    }
    g_osd_span_rows = idx * FONT_CHAR_HEIGHT;
    // The solid menu box spans the widest item, centered like the items, so
    // the paint can fill one rectangle (hiding the zoom/pan/blur boundary
    // behind it) instead of a full-width band or per-item stair-steps.
    uint32_t box_w = 0;
    for (uint32_t i = 0; i < idx; i++) {
        if (g_osd_w[i] > box_w) {
            box_w = g_osd_w[i];
        }
    }
    g_osd_box_w = (uint16_t)box_w;
    g_osd_box_x0 = (uint16_t)((FRAME_WIDTH - box_w) / 2);
    // Pre-render each box row to color (480P layout: one word = two pixels),
    // so the scanline paint becomes one memcpy per line. Same bit-to-color
    // mapping as the paint pass it replaces.
    if (box_w > OSD_BOX_WORDS_MAX * 2) {
        box_w = OSD_BOX_WORDS_MAX * 2;
        g_osd_box_w = (uint16_t)box_w;
        g_osd_box_x0 = (uint16_t)((FRAME_WIDTH - box_w) / 2);
    }
    uint32_t box_words = box_w >> 1;
    uint32_t bxo = g_osd_box_x0;
    for (uint32_t r = 0; r < g_osd_span_rows; r++) {
        const uint8_t *bits = g_osd_bitmap[r];
        const uint16_t fg = g_osd_fg[r >> 3];
        const uint16_t bg = g_osd_bg[r >> 3];
        for (uint32_t w = 0; w < box_words; w++) {
            uint32_t xa = bxo + (w << 1);
            uint32_t pa = (bits[xa >> 3] >> (7 - (xa & 7))) & 1u;
            uint32_t pb = (bits[(xa + 1) >> 3] >> (7 - ((xa + 1) & 7))) & 1u;
            g_osd_box_buf[r][w] = (pa ? fg : bg) | ((uint32_t)(pb ? fg : bg) << 16);
        }
    }
    // Clear any stale rows beyond the current menu height.
    if (g_osd_span_rows < OSD_MENU_ROW_SPAN) {
        memset(&g_osd_bitmap[g_osd_span_rows][0], 0,
               (OSD_MENU_ROW_SPAN - g_osd_span_rows) * OSD_BITMAP_BYTES_PER_ROW);
    }
}
#endif // N64_HDMI_OSD_OVERLAY

void osd_run(void)
{
    // Get Joybus state to decide if we should show the menu
    uint32_t buttons = joybus_rx_get_latest();
    if (OSD_SHORTCUT(buttons)) {
        state.open = true;

        // Reset menu state
        state.last_buttons = buttons;
        state.root = menu;
        state.current_root = menu;
        state.focused_item = NULL;
        memset(state.stack, 0, sizeof(state.stack));
    }

#ifdef N64_HDMI_OSD_OVERLAY
    // Non-blocking: one input event + redraw per call, so the capture loop
    // keeps filling g_framebuf with live video while the menu is open. The
    // scanline callback paints the menu bitmap over that live picture.
#ifdef N64_HDMI_OSD_FORCE_OPEN
    if (osd_force_open_once() && !state.open) {
        state.open = true;
        state.root = menu;
        state.current_root = menu;
        state.focused_item = NULL;
        memset(state.stack, 0, sizeof(state.stack));
    }
#endif
    if (!state.open) {
        return;
    }

    if (state.focused_item == NULL) {
        state.focused_item = state.current_root;
    }

    // Handle Input (edge-triggered, one step per press)
    if (BUTTON_PRESSED(DD_BUTTON)) {
        menu_item_t *n = state.focused_item + 1;
        while (n->text != NULL && !osd_item_visible(n)) {
            n++;
        }
        if (n->text != NULL) {
            state.focused_item = n;
        }
    }
    else if (BUTTON_PRESSED(DU_BUTTON)) {
        if (state.focused_item != state.current_root) {
            menu_item_t *n = state.focused_item - 1;
            while (n != state.current_root && !osd_item_visible(n)) {
                n--;
            }
            // current_root ("OSD Menu") is always visible, so n is a valid
            // landing spot regardless.
            state.focused_item = n;
        }
    }
    else if (BUTTON_PRESSED(DR_BUTTON) || BUTTON_PRESSED(DL_BUTTON)) {
        menu_item_t *it = state.focused_item;
        bool inc = BUTTON_PRESSED(DR_BUTTON);
        if (it->type == ITEM_TYPE_VALUE_RW_U32) {
            int64_t nv = (int64_t)*it->value.value_u32 + (inc ? it->step : -it->step);
            if (nv > it->max) nv = it->max;
            if (nv < it->min) nv = it->min;
            *it->value.value_u32 = (uint32_t)nv;
            if (it->on_change) it->on_change();
        } else if (it->type == ITEM_TYPE_VALUE_RW_I32) {
            int32_t nv = *it->value.value_i32 + (inc ? it->step : -it->step);
            if (nv > it->max) nv = it->max;
            if (nv < it->min) nv = it->min;
            *it->value.value_i32 = nv;
            if (it->on_change) it->on_change();
        }
    }
    else if (BUTTON_PRESSED(A_BUTTON)) {
        menu_item_t *it = state.focused_item;
        if (it->type == ITEM_TYPE_MENU) {
            state.current_root = (menu_item_t *)it->value.value_ptr;
            state.focused_item = NULL;
        }
        else if (it->type == ITEM_TYPE_BACK) {
            state.current_root = (menu_item_t *)state.current_root->value.value_ptr;
            state.focused_item = NULL;
        }
        else if (it->type == ITEM_TYPE_EXIT) {
            state.open = false;
        }
        else if (it->type == ITEM_TYPE_ACTION) {
            if (it->on_change) {
                it->on_change();
            }
            state.open = false;
        }
    }
    else if (BUTTON_PRESSED(B_BUTTON)) {
        // B closes the menu (the README's "B or the shortcut").
        state.open = false;
    }

    state.last_buttons = buttons;

    // Keep the overlay current: values/focus change as input is handled.
    if (state.open) {
        osd_render();
    }
#else
    bool rerender = false;

    // Show OSD menu
    while (state.open && !rerender) {
        menu_item_t *item = state.current_root;
        uint32_t y = OSD_Y_OFFSET;
        uint32_t x = OSD_X_OFFSET;

        if (state.focused_item == NULL) {
            state.focused_item = item;
        }

        // Render
        while (item && item->text) {
            if (!osd_item_visible(item)) {
                item++;
                continue;
            }
            uint16_t bg_color = (item == state.focused_item) ? (RGB888_TO_RGB565(0xff, 0x00, 0xff)) : (RGB888_TO_RGB565(0x00, 0x00, 0x00));
            uint16_t fg_color = RGB888_TO_RGB565(0xff, 0xff, 0xff);

            if (item->type == ITEM_TYPE_VALUE_RW_U32) {
                if (item->value.value_u32 == &g_config.output_mode) {
                    const char *m = *item->value.value_u32 == 2 ? "720P"
                                  : *item->value.value_u32 == 1 ? "480P"
                                                                : "240P";
                    gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %s",
                        item->text, m);
                } else if (item->value.value_u32 == &g_config.aspect_ratio) {
                    gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %s",
                        item->text, *item->value.value_u32 ? "4:3" : "16:9");
                } else if (item->value.value_u32 == &g_config.video_region) {
                    const char *r = *item->value.value_u32 == 2 ? "PAL"
                                  : *item->value.value_u32 == 1 ? "NTSC"
                                                                : "Auto";
                    gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %s",
                        item->text, r);
                } else if (item->value.value_u32 == &g_config.blur_enabled) {
                    gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %s",
                        item->text, *item->value.value_u32 ? "ON" : "OFF");
                } else {
                    gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %lu",
                        item->text, (unsigned long)*item->value.value_u32);
                }
            } else if (item->type == ITEM_TYPE_VALUE_RW_I32) {
                gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s: %ld",
                    item->text, (long)*item->value.value_i32);
            } else {
                gfx_puttextf(x, y++ * 8, bg_color, fg_color, "%s", item->text);
            }
            item++;
        }

        // Handle Input
        buttons = joybus_rx_get_latest();
        if (BUTTON_PRESSED(DD_BUTTON)) {
            menu_item_t *n = state.focused_item + 1;
            while (n->text != NULL && !osd_item_visible(n)) {
                n++;
            }
            if (n->text != NULL) {
                state.focused_item = n;
            }
        }
        else if (BUTTON_PRESSED(DU_BUTTON)) {
            if (state.focused_item != state.current_root) {
                menu_item_t *n = state.focused_item - 1;
                while (n != state.current_root && !osd_item_visible(n)) {
                    n--;
                }
                state.focused_item = n;
            }
        }
        else if (BUTTON_PRESSED(DR_BUTTON) || BUTTON_PRESSED(DL_BUTTON)) {
            menu_item_t *it = state.focused_item;
            bool inc = BUTTON_PRESSED(DR_BUTTON);
            if (it->type == ITEM_TYPE_VALUE_RW_U32) {
                int64_t nv = (int64_t)*it->value.value_u32 + (inc ? it->step : -it->step);
                if (nv > it->max) nv = it->max;
                if (nv < it->min) nv = it->min;
                *it->value.value_u32 = (uint32_t)nv;
                if (it->on_change) it->on_change();
            } else if (it->type == ITEM_TYPE_VALUE_RW_I32) {
                int32_t nv = *it->value.value_i32 + (inc ? it->step : -it->step);
                if (nv > it->max) nv = it->max;
                if (nv < it->min) nv = it->min;
                *it->value.value_i32 = nv;
                if (it->on_change) it->on_change();
            }
        }
        else if (BUTTON_PRESSED(A_BUTTON)) {
            if (state.focused_item->type == ITEM_TYPE_MENU) {
                menu_item_t *previous_root = state.current_root;
                state.current_root = (menu_item_t *) state.focused_item->value.value_ptr;
                state.current_root->value.value_ptr = previous_root;
                state.focused_item = NULL;
                rerender = true;
            }
            else if (state.focused_item->type == ITEM_TYPE_BACK) {
                state.current_root = (menu_item_t *) state.current_root->value.value_ptr;
                state.focused_item = NULL;
                rerender = true;
            }
            else if (state.focused_item->type == ITEM_TYPE_EXIT) {
                state.open = false;
            }
            else if (state.focused_item->type == ITEM_TYPE_ACTION) {
                if (state.focused_item->on_change) {
                    state.focused_item->on_change();
                }
                state.open = false;
            }
        }
        else if (BUTTON_PRESSED(B_BUTTON)) {
            // B closes the menu (the README's "B or the shortcut").
            state.open = false;
        }

        state.last_buttons = buttons;
    }
#endif // N64_HDMI_OSD_OVERLAY
}
