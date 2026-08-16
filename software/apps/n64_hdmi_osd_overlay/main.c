/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann      (original N64 digital-video capture)
 * Copyright (c) 2026 - HSTX/pico_hdmi port
 *
 * This is PicoDVI-N64's capture/OSD/joybus logic, unchanged, driving
 * pico_hdmi's HSTX-based HDMI backend instead of the original PIO
 * bit-banged libdvi. See README.md in the repo root for the rationale
 * and a list of the few things that had to change.
 */

#pragma GCC optimize("O3")

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/io_qspi.h"
#include "hardware/structs/sio.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/hstx_pins.h"
#include "pico_hdmi/video_output_rt.h"

#include "joybus.h"
#include "joybus.pio.h"
#include "n64.pio.h"

#include "gfx.h"
#include "osd.h"
#include "app.h"

// Enable to print debug/diagnostics
// #define DIAGNOSTICS
// #define DIAGNOSTICS_JOYBUS

// ----------------------------------------------------------------------------
// Pinout reference (unchanged from the original board/wiring - see
// hardware/board/*.kicad_pcb in the original PicoDVI-N64 repo)
// ----------------------------------------------------------------------------
#define PIN_VIDEO_D0     0
#define PIN_VIDEO_D1     1
#define PIN_VIDEO_D2     2
#define PIN_VIDEO_D3     3
#define PIN_VIDEO_D4     4
#define PIN_VIDEO_D5     5
#define PIN_VIDEO_D6     6
#define PIN_VIDEO_DSYNC  7
#define PIN_VIDEO_CLK    8
#define PIN_AUDIO_LRCLK  9
#define PIN_AUDIO_SDAT  10
#define PIN_AUDIO_BCLK  11
#define PIN_JOYBUS_P1   20

// GPIO12-19 are the HSTX-capable TMDS pins on RP2350. The CMake preset
// N64_HDMI_PINOUT selects which physical GPIO pair carries each TMDS pair:
//
//   - pico_hdmi: pico_hdmi's own default pinout (CLK-=12/CLK+=13,
//     D0-=14/D0+=15, D1-=16/D1+=17, D2-=18/D2+=19), which matches this
//     board's HSTX wiring (the PicoDVI-N64-style board). No
//     video_output_set_hstx_pinout() call is needed. NOT the old
//     dvi_serialiser "pico_sock_cfg" order (CLK=14, D0=12, D1=18, D2=16)
//     that the original PicoDVI-N64 board used. (An earlier version of this
//     file added a translated custom pinout assuming the old dvi_serialiser
//     order - that was wrong for this board's actual wiring and was the
//     cause of a "no video output" report; removed.)
//
//   - dvisock: Pimoroni Pico DVI Sock. GP12=D0+, GP13=D0-, GP14=CK+,
//     GP15=CK-, GP16=D2+, GP17=D2-, GP18=D1+, GP19=D1- (the same lane
//     order as the old dvi_serialiser "pico_sock_cfg" above). main()
//     applies it via video_output_set_hstx_pinout() below.

#ifdef N64_HDMI_PINOUT_DVI_SOCK
static const pico_hdmi_hstx_pinout_t dvi_sock_pinout = {
    .clock = {.positive_gpio = 14, .negative_gpio = 15},
    .data = {{.positive_gpio = 12, .negative_gpio = 13},
             {.positive_gpio = 18, .negative_gpio = 19},
             {.positive_gpio = 16, .negative_gpio = 17}},
};
#endif

#define IN_RANGE(__x, __low, __high) (((__x) >= (__low)) && ((__x) <= (__high)))
#define IN_TOLERANCE(__x, __value, __tolerance) IN_RANGE(__x, (__value - __tolerance), (__value + __tolerance))

// ----------------------------------------------------------------------------
// N64_HDMI_OUTPUT_640x480: custom 480p video_mode_t, run at 252 MHz sys_clk
// / VREG 1.15V with hstx_clk_div=2 (-> 252/2/5 = 25.2 MHz pixel clock,
// electrically identical to the library's stock video_mode_480_p at
// 126 MHz sys_clk / hstx_clk_div=1). This exact struct + clock/voltage
// sequence (see main(), below) was confirmed working on this hardware in
// a standalone test; the library's own video_mode_480_p was not
// separately re-tested with this hardware/wiring, so this project uses
// the config with an actual positive confirmation rather than assuming
// the library default behaves identically here.
// Always defined (not just 480p builds): the OSD can switch 240P<->480P at
// runtime via video_output_set_mode(), so both mode descriptors must exist
// in every build.
//
// Standard CEA/VGA 800x525 timing (v_total_lines = 525, 60.000 Hz) -- the
// EXACT descriptor apps/n64_diag_noskip uses, which is confirmed clean on
// this hardware. (An experiment setting 524 lines to match the 240P
// descriptor's 60.115 Hz, to remove single-buffer tear drift, changed
// nothing: the 480P rolling band was present at both 525 and 524, while
// n64_diag_noskip at 525 is clean -- so the artifact is NOT the frame rate.)
const video_mode_t n64_hdmi_480p_confirmed = {
    .h_front_porch = 16,
    .h_sync_width = 96,
    .h_back_porch = 48,
    .h_active_pixels = 640,

    .v_front_porch = 10,
    .v_sync_width = 2,
    .v_back_porch = 33,
    .v_active_lines = 480,

    .h_total_pixels = 800,
    .v_total_lines = 525,

    .hstx_clk_div = 2,
    .hstx_csr_clkdiv = 5,

    .hsync_positive = false,
    .vsync_positive = false,

    .data_island_in_hsync = true,
};

// 240p twin of n64_hdmi_480p_confirmed: the library's video_mode_240_p uses
// hstx_clk_div = PICO_HDMI_240P_HSTX_CLK_DIV, which is 1 unless
// PICO_HDMI_240P_HSTX_CLK_DIV2 is enabled on the library (the OC252 build
// does that). The 480p builds also run at 252 MHz sys_clk, so booting them in
// 240p mode with the stock div=1 descriptor produces a doubled 50.4 MHz pixel
// clock (a nonstandard ~120 Hz 240p signal that sinks show at the limit and
// drop when the OSD overlay's per-word box blit overruns the scanline IRQ
// budget). This descriptor keeps the pixel clock at 25.2 MHz (252/2/5),
// electrically identical to the stock 240p at 126 MHz, for every build that
// raises sys_clk.
const video_mode_t n64_hdmi_240p_confirmed = {
    .h_front_porch = 32,
    .h_sync_width = 192,
    .h_back_porch = 96,
    .h_active_pixels = 1280,

    .v_front_porch = 4,
    .v_sync_width = 4,
    .v_back_porch = 14,
    .v_active_lines = 240,

    .h_total_pixels = 1600,
    .v_total_lines = 262,

    .hstx_clk_div = 2,
    .hstx_csr_clkdiv = 5,

    .hsync_positive = false,
    .vsync_positive = false,

    .data_island_in_hsync = true,
};

void app_save_config_and_reboot(void)
{
    config_save();
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

#define N64_RESET_GPIO 21

// ----------------------------------------------------------------------------
// N64 reset watchdog ("Resetting..." overlay)
//
// app_reset_n64() only asserts the reset line; the actual waiting happens in
// the video capture loop on Core 0, which is the only place that can tell
// whether the N64's video signal has come back. While the reset is in
// progress the frame buffer holds a solid black frame plus a centered
// "Resetting" label with animated dots, which the scanline callback on
// Core 1 keeps outputting; the picture returns once VSYNC has been lost and
// then a complete fresh frame has been captured.
//
// Sequence: reset asserted -> VSYNC disappears -> 150 ms without sync marks
// the N64 as really down (overlay animates) -> VSYNC returns -> the first
// complete frame is captured -> overlay clears. A timeout gives up and
// returns to normal output if the console never stops sending video.
// ----------------------------------------------------------------------------
#define N64_RESET_SYNC_LOST_US (150 * 1000u)     // video considered gone
#define N64_RESET_MAX_US       (10000 * 1000u)   // give up after 10 s
#define N64_RESET_ANIM_US      (250 * 1000u)     // one dot state per 250 ms
#define N64_RESET_NO_DROP_US   (300 * 1000u)     // no video drop seen: assume reset done

// While a reset is pending, any blocking sample wait that exceeds the
// sync-lost threshold must give up and return to the outer loop (which
// animates the "Resetting..." overlay and waits for VSYNC again) instead
// of spinning forever on a FIFO that stopped receiving samples.
#define N64_RESET_BAIL_ON_SYNC_LOSS() \
    do { \
        if (n64_reset_active && ((time_us_32() - last_sync_us) > N64_RESET_SYNC_LOST_US)) { \
            goto end_of_line; \
        } \
    } while (0)

static uint32_t last_sync_us;         // when the last VSYNC was seen
static bool n64_reset_active;         // reset requested and not yet finished
static uint32_t n64_reset_start_us;   // when the reset was requested
static bool n64_reset_sync_was_lost;  // video actually disappeared
static bool n64_reset_sync_back;      // ...and came back (fresh frame pending)
static uint32_t n64_reset_last_us;    // when the last reset was issued

static void draw_reset_overlay(uint32_t ms)
{
    static const char *const dots[] = {"", ".", "..", "..."};
    char text[16];
    snprintf(text, sizeof(text), "Resetting%s", dots[(ms / N64_RESET_ANIM_US) % 4]);
    memset(g_framebuf, 0x00, sizeof(g_framebuf));
    // "Resetting" is 9 chars * 8 px = 72 px wide; centered.
    gfx_puttext((FRAME_WIDTH - 9 * 8) / 2, (FRAME_HEIGHT - 8) / 2,
                RGB888_TO_RGB565(0x00, 0x00, 0x00),
                RGB888_TO_RGB565(0xff, 0xff, 0xff), text);
}

// Reset the N64 from the OSD. The N64's reset line is ACTIVE-LOW with a
// pull-up on the console's board: it must never be driven HIGH, doing so
// can damage the console. To assert reset we float the pin first, stage the
// output latch low, switch to output (the pin goes low), hold it for
// 100 ms, then float it again so the console's pull-up restores it high.
// A re-reset within 5 s is ignored so the boot/PIF handshake can finish.
// The screen stays black with a "Resetting..." overlay until the capture
// loop sees fresh video again.
void app_reset_n64(void)
{
    uint32_t now = time_us_32();
    if ((now - n64_reset_last_us) < 5 * 1000000u) {
        return; // too soon after the previous reset
    }
    n64_reset_last_us = now;

    n64_reset_start_us = now;
    last_sync_us = n64_reset_start_us; // fresh baseline for loss detection
    n64_reset_sync_was_lost = false;
    n64_reset_sync_back = false;
    n64_reset_active = true;
    draw_reset_overlay(0);

    gpio_init(N64_RESET_GPIO);                // SIO input, no pulls: floats
    gpio_put(N64_RESET_GPIO, 0);              // stage output low while input
    gpio_set_dir(N64_RESET_GPIO, GPIO_OUT);   // pin goes low: reset asserted
    busy_wait_ms(100);
    gpio_set_dir(N64_RESET_GPIO, GPIO_IN);    // float: pull-up restores high
}

// ----------------------------------------------------------------------------
// BOOTSEL button read (RP2350)
//
// On Pico 2 the BOOTSEL button is wired to the flash chip's /CS pad
// (QSPI_CSN) - the same signal the bootrom checks. Reading it as a button
// means briefly reconfiguring that pad as an input, which suspends flash
// access, so the read must run from RAM with interrupts off and the other
// core parked. That is exactly the situation at boot, before any PIO/DMA
// starts: holding BOOTSEL at power-on is a reliable "factory reset".
// ----------------------------------------------------------------------------
#if defined(PICO_RP2040)
#define BOOTSEL_CS_BIT (1u << 1)
#else
#define BOOTSEL_CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif

static bool __no_inline_not_in_flash_func(bootsel_button_is_pressed)(void)
{
    // Disable interrupts: IRQ handlers may live in flash, which we are
    // about to make temporarily inaccessible.
    uint32_t flags = save_and_disable_interrupts();

    // Float the flash chip-select (make it an input) so the button's
    // pull-down is observable.
    hw_write_masked(&io_qspi_hw->io[1].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // No flash access from here on: spin, don't call any sleep function.
    for (volatile int i = 0; i < 1000; ++i);

    // The button pulls the pad low when pressed.
    bool pressed = !(sio_hw->gpio_hi_in & BOOTSEL_CS_BIT);

    // Restore normal chip-select drive before touching flash again.
    hw_write_masked(&io_qspi_hw->io[1].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}


// ----------------------------------------------------------------------------
// PIO assignment
//
// The old code had to share PIO0 with the DVI serialiser's TMDS state
// machines (3 of PIO0's 4 SMs, leaving 1 free for Joybus). HSTX is a
// separate hardware peripheral, not a PIO client, so both PIOs are now
// completely free for capture:
//   PIO0 SM0 - Joybus RX
//   PIO1 SM0 - N64 digital video capture
//   PIO1 SM1 - N64 digital audio (I2S-like) capture
// ----------------------------------------------------------------------------
const PIO pio_joybus = pio0;
const uint sm_joybus = 0;

const PIO pio_n64 = pio1;
const uint sm_video = 0;
const uint sm_audio = 1;

// ----------------------------------------------------------------------------
// Audio capture -> HDMI Data Island forwarding
//
// Unchanged from the original: a DMA job continuously latches the
// freshest word out of the audio PIO's RX FIFO into `last_audio_sample`,
// and a DMA timer paced at the configured output sample rate copies that
// latest sample into a ring buffer (`audio_buffer`). This decouples the
// N64's own audio bit-clock from the HDMI audio sample rate we advertise.
//
// What's new: instead of handing the ring buffer to libdvi's audio_ring
// consumer, a pico_hdmi "background task" (run continuously on Core 1,
// between scanline services) walks the ring 4 samples at a time and
// pushes pre-encoded HDMI audio Data Islands into pico_hdmi's queue.
// ----------------------------------------------------------------------------
audio_sample_t      last_audio_sample;
audio_sample_t      audio_buffer[AUDIO_BUFFER_SIZE];

static uint g_dma_ch_audio_buffer_data;
#ifdef N64_HDMI_AUDIO_ONLY
// Static SMPTE-ish bar pattern for the audio-only test build: no video
// capture runs, so the scanline callback draws this instead of g_framebuf.
static uint16_t test_pattern[FRAME_WIDTH];
#endif
#if AUDIO_ENABLED
#if !defined(N64_HDMI_AUDIO_SILENT)
static uint32_t audio_read_index = 0;
#endif
static int audio_frame_counter = 0;
#endif

static inline uint32_t audio_buffer_write_index(void)
{
    // The capture DMA writes forward through audio_buffer[] and its
    // write_addr register always points at the *next* word it will
    // write. Turning that back into an index gives us "how far the
    // producer has gotten" without needing an IRQ per sample.
    uintptr_t base = (uintptr_t)audio_buffer;
    uintptr_t cur  = (uintptr_t)dma_hw->ch[g_dma_ch_audio_buffer_data].write_addr;
    return ((uint32_t)(cur - base) / sizeof(audio_sample_t)) & (AUDIO_BUFFER_SIZE - 1);
}

#if AUDIO_ENABLED
static void hdmi_audio_task(void)
{
    // No UART here: any printf() inside the execution path stalls Core 1's
    // HSTX line service and makes sinks drop the signal (confirmed on this
    // hardware; same class of bug as the old printf() inside the capture
    // loop). Startup checkpoints only, before the capture loop starts.
#if defined(N64_HDMI_AUDIO_SILENT)
    // Isolation build: feed the DI queue directly with silence packets, no
    // N64 audio capture (PIO SM1 / DMA ring) runs. Keep the queue a little
    // ahead so the ISR's get_audio_packet() finds real islands to patch in.
    if (hstx_di_queue_get_level() >= 16) {
        return;
    }
    audio_sample_t samples[4] = {0};
    hstx_packet_t packet;
    audio_frame_counter = hstx_packet_set_audio_samples_cs_rate(
        &packet, samples, 4, audio_frame_counter, g_config.audio_out_sample_rate);
    hstx_data_island_t island;
    hstx_encode_data_island(&island, &packet, false, hstx_di_queue_get_hsync_active());
    hstx_di_queue_push(&island);
#else
    uint32_t write_index = audio_buffer_write_index();
    // Push complete groups of 4 samples as they become available. Cap the
    // amount of work per call so a long stall elsewhere can't make this
    // loop hog Core 1 catching up all at once.
    int budget = 64;
    while (budget-- > 0) {
        uint32_t available = (write_index - audio_read_index) & (AUDIO_BUFFER_SIZE - 1);
        if (available < 4) {
            break;
        }

        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            samples[i] = audio_buffer[(audio_read_index + i) & (AUDIO_BUFFER_SIZE - 1)];
        }
        audio_read_index = (audio_read_index + 4) & (AUDIO_BUFFER_SIZE - 1);

        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples_cs_rate(
            &packet, samples, 4, audio_frame_counter, g_config.audio_out_sample_rate);

        hstx_data_island_t island;
        // hstx_di_queue_get_hsync_active() (not a fixed DI_HSYNC_ACTIVE
        // macro) because the runtime API's video_output_set_mode() sets
        // this per-mode at runtime (from video_mode_t.data_island_in_hsync),
        // not at compile time.
        hstx_encode_data_island(&island, &packet, false, hstx_di_queue_get_hsync_active());
        if (!hstx_di_queue_push(&island)) {
            // Queue is full; try again next call.
            audio_read_index = (audio_read_index - 4) & (AUDIO_BUFFER_SIZE - 1);
            break;
        }
    }
#endif // N64_HDMI_AUDIO_SILENT
}
#endif

// ----------------------------------------------------------------------------
// Video: scanline callback, called from Core 1 (pico_hdmi/HSTX side) each
// time it needs a fresh line. This is fully decoupled from the N64
// capture loop below (same tradeoff the original code made: no genlock
// between the N64's video timing and the HDMI output timing, so the
// output always shows the freshest available g_framebuf line - occasional
// tearing is possible but there is no visible judder/frame-doubling
// logic to get wrong).
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Scanline Callback (Core 0 DMA ISR) - the video path. Keep this fast: it
// runs inside the scanline IRQ, which preempts the N64 capture loop on Core
// 0. No UART here (UART in execution drops the sink's signal on this
// hardware), no volatile counters, nothing that adds per-line work.
// ----------------------------------------------------------------------------
// Unified mapper for both output modes and OSD zoom/pan/blur:
//   - 480P: 640 output px/line, 480 lines -> vertical 2:1 from the 240-line
//     capture (as before).
//   - 240P: 1280 output px/line (each of the 640 captured samples repeated),
//     240 lines -> 1:1 vertical.
//   - 720P: 1280 output px/line (2x horizontal repeat, same layout as 240P),
//     720 lines -> vertical 3:1 from the 240-line capture. With
//     g_config.aspect_ratio = 4:3 the picture is drawn into a centered
//     960px region with black side bars instead (scanline_map_square()).
//   - g_config.zoom_percent / pos_x / pos_y pick a viewport into the 640x240
//     capture. The common case (zoom=100, no offset) uses a memcpy fast path
//     identical to the original callbacks; only when zoom/pan is active does
//     it step through the viewport in fixed-point 16.16.
//   - g_config.blur_enabled softens the image horizontally in both output
//     modes (the axis the N64's dithering stripes show on) and applies the
//     same 50/50 blend while zoomed.
static inline uint16_t blend16(uint16_t a, uint16_t b)
{
    // 50/50 RGB565 average (mask channels to avoid carry between them).
    return (uint16_t)(((a & 0xF7DEu) + (b & 0xF7DEu)) >> 1);
}

static inline uint32_t load16x2(const uint16_t *p)
{
    // Two consecutive RGB565 pixels as one 32-bit word (alignment is
    // guaranteed by the 1280-byte row stride and even x). memcpy instead of
    // an aliasing cast: GCC turns a 4-byte memcpy into a single load.
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void scanline_general_fill(const uint16_t *src, uint32_t *dst,
                                  int32_t acc, uint32_t step, uint32_t w0, uint32_t w1)
{
    for (uint32_t w = w0; w < w1; w++) {
        int32_t idx = acc >> 16;
        uint32_t p0 = src[idx];
        acc += step;
        idx = acc >> 16;
        uint32_t p1 = src[idx];
        acc += step;
        dst[w] = p0 | (p1 << 16);
    }
}

// 1280-wide (240P/720P) general viewport fill: each output word shows ONE
// source sample twice (p | p << 16) - the picture is a 2x horizontal repeat
// of the 640-sample capture, so this matches the zoom=100 fast path exactly.
// The old shared 640-wide path sampled TWO source pixels per word (correct
// only for 480P), which at 720P doubled the transform cost (HSTX underflow ->
// sink dropped the signal when changing Zoom/Pos/Blur) AND shifted every
// sampled word half a pixel right of the fast-path layout. Blur blends each
// sample with its right neighbour, the same filter the 1280-wide fast path
// uses (neighbour clamps to self at the right edge).
static void scanline_general_fill_wide(const uint16_t *src, uint32_t *dst,
                                       int32_t acc, uint32_t step,
                                       uint32_t w0, uint32_t w1, bool blur)
{
    if (blur) {
        for (uint32_t w = w0; w < w1; w++) {
            uint32_t s = (uint32_t)(acc >> 16);
            acc += (int32_t)step;
            uint32_t nx = s + 1;
            if (nx >= FRAME_WIDTH) nx = s;
            uint32_t q = blend16(src[s], src[nx]);
            dst[w] = q | (q << 16);
        }
    } else {
        for (uint32_t w = w0; w < w1; w++) {
            uint32_t p = src[acc >> 16];
            acc += (int32_t)step;
            dst[w] = p | (p << 16);
        }
    }
}

// Blit a pre-rendered menu box row over the output words [c0, c1). Called at
// the end of every fill path in scanline_map() so the OSD stays on top of the
// transform everywhere. The pre-rendered row is always in 480P layout (one
// word = two pixels, box word j = source pixels [box_x0+2j, box_x0+2j+1));
// in 480P the box rectangle maps 1:1 to words ([c0,c1) = [box_x0/2, ...) and
// the memcpy lands each word in place). In 240P it covers
// [box_x0, box_x0+box_w) output words (one word = ONE pixel, repeated pair,
// so output word w shows source pixel w): box_x0 is even and box_w a
// multiple of 8, so output word w maps to box word (w - box_x0) >> 1 (the
// index is relative to the box start, NOT absolute - the row buffer is
// indexed from 0) and each 480P word is expanded into two output words with
// the pixel replicated.
static void osd_box_blit(uint32_t *dst, uint32_t c0, uint32_t c1, const uint32_t *src)
{
    if (video_output_active_mode->h_active_pixels == FRAME_WIDTH) {
        memcpy(&dst[c0], src, (c1 - c0) * sizeof(uint32_t));
    } else {
        for (uint32_t w = c0; w < c1; w++) {
            uint32_t e = src[(w - c0) >> 1];
            uint32_t c = (w & 1u) ? (e >> 16) : (e & 0xFFFFu);
            dst[w] = c | (c << 16);
        }
    }
}

// One 480P block-blur output pair: dst[w], dst[w+1] = average of source
// pixel pairs at x and x+2 (see the zoom=100 fast path). Split into a helper
// so the box region can be skipped in the middle of the blur line without
// duplicating the filter math.
static void blur_block_pair(const uint16_t *src, uint32_t x, uint32_t *dst, uint32_t w)
{
    uint32_t a0 = load16x2(&src[x]);
    uint32_t a1 = load16x2(&src[x + 2]);
    uint32_t q0 = ((a0 >> 1) & 0x7BEFu) + ((a0 >> 17) & 0x7BEFu);
    uint32_t q1 = ((a1 >> 1) & 0x7BEFu) + ((a1 >> 17) & 0x7BEFu);
    dst[w] = q0 | (q0 << 16);
    dst[w + 1] = q1 | (q1 << 16);
}

static void scanline_map(uint32_t active_line, uint32_t *dst,
                         uint32_t clip_w0, uint32_t clip_w1, const uint32_t *clip_src)
{
    const bool has_clip = (clip_src != NULL) && (clip_w0 < clip_w1);
#ifdef N64_HDMI_AUDIO_ONLY
    (void)active_line;
    memcpy(dst, test_pattern, FRAME_WIDTH * sizeof(uint16_t));
    return;
#endif

    const video_mode_t *mode = video_output_active_mode;
    const uint32_t active_px = mode->h_active_pixels;   // 640 (480P) or 1280 (240P/720P)
    const uint32_t active_lines = mode->v_active_lines; // 480, 240 or 720
    const bool blur = g_config.blur_enabled;
    // Source row for this output line (the 240-line capture is scaled up):
    // 480P 2:1 (active_line >> 1), 720P 3:1 (active_line / 3), 240P 1:1.
    const uint32_t src_row = (active_px == FRAME_WIDTH)
        ? (active_line >> 1)
        : ((active_lines == 720U) ? (active_line / 3) : active_line);

    // Viewport into the 640x240 capture (clamped; never trust config blindly).
    // zoom_percent: 100 = full frame, >100 = magnify, <100 = zoom out (the
    // picture shrinks toward the center with black bars around it - there is
    // nothing "outside" the full capture, so zooming out just scales it down).
    // pos_x/pos_y are signed offsets from the CENTER of the frame, so any
    // zoom expands/shrinks around the center. pos may push the viewport past
    // the source edge: out-of-range samples are drawn black, so panning
    // shifts the picture (with a black bar) even at zoom=100.
    uint32_t zoom = g_config.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 400) zoom = 400;
    uint32_t vw = (FRAME_WIDTH * 100u) / zoom;   // > FRAME_WIDTH when zoom < 100
    uint32_t vh = (FRAME_HEIGHT * 100u) / zoom;  // > FRAME_HEIGHT when zoom < 100
    if (vw == 0) vw = 1;
    if (vh == 0) vh = 1;
    int32_t vx = ((int32_t)FRAME_WIDTH - (int32_t)vw) / 2 + g_config.pos_x;
    // While the menu is open the GAME picture (NOT the menu box) is panned
    // down: opening the menu makes the game appear to jump a few rows (the
    // capture and HDMI output share one framebuffer and are not
    // phase-locked), so a vertical pan puts it back. The amount is per output
    // mode (OSD_OPEN_Y_SHIFT_720P vs OSD_OPEN_Y_SHIFT); the OSD "Pos Y" item
    // still shows the raw config value (0).
    int32_t vy = ((int32_t)FRAME_HEIGHT - (int32_t)vh) / 2
        + g_config.pos_y - (osd_is_open()
            ? (int32_t)((active_lines == 720U) ? OSD_OPEN_Y_SHIFT_720P : OSD_OPEN_Y_SHIFT)
            : 0);
    // Generous clamp: the viewport may slide fully off the source (black), but
    // keep the 16.16 accumulator far away from int32 overflow.
    if (vx < -(int32_t)vw) vx = -(int32_t)vw;
    if (vx > (int32_t)FRAME_WIDTH) vx = FRAME_WIDTH;
    if (vy < -(int32_t)vh) vy = -(int32_t)vh;
    if (vy > (int32_t)FRAME_HEIGHT) vy = FRAME_HEIGHT;

    if (vw == FRAME_WIDTH && vh == FRAME_HEIGHT && vx == 0) {
        // Fast path: full frame, no zoom/horizontal pan. A pure vertical pan
        // (vy != 0) is allowed: the source row is offset and off-source rows
        // become black bars, keeping this cheaper than the general path (which
        // overran the 480P scanline IRQ budget when Pos Y != 0).
        if (active_px == FRAME_WIDTH) {
            // 480P: 1:1 horizontal, 2:1 vertical from the 240-line capture.
            int32_t r0i = (int32_t)src_row + vy;
            if (r0i < 0 || r0i >= (int32_t)FRAME_HEIGHT) {
                // Whole line off the source: solid black (OSD box still on top).
                memset(dst, 0, (active_px >> 1) * sizeof(uint32_t));
                if (has_clip) {
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                }
                return;
            }
            uint32_t r0 = (uint32_t)r0i;
            if (blur) {
                // Horizontal softening, same filter as the 240P fast path:
                // each 2-pixel block is replaced by its average. At 480P the
                // output is 1:1 horizontally, so this is the axis the N64's
                // column-stripe dithering shows on - a vertical blend leaves
                // it untouched because every row carries the same stripes
                // (and mixing rows of opposite fields is wrong for 480i
                // sources). Averaging 2 pixels in one 32-bit op keeps this
                // within the scanline IRQ budget: the 0xF7DE mask per half
                // stops inter-channel carries and clears bit 0, so the
                // packed sum is even and the >>1 average is exact.
                const uint16_t *src = &g_framebuf[FRAME_WIDTH * r0];
                if (has_clip) {
                    // Skip the OSD box words and blit them in the MIDDLE of the
                    // line: this blur path is the most expensive fill (~3.5k
                    // cycles), so writing the box late (at the very end) pushed
                    // the line against the scanline IRQ budget and the HSTX
                    // underflowed around the middle of the line, damaging the
                    // right half of the box. Skipping the box also drops the
                    // fill cost by ~10 cycles per skipped word.
                    uint32_t words = active_px >> 1;
                    uint32_t x, w;
                    for (x = 0, w = 0; w < clip_w0; w += 2, x += 4) {
                        blur_block_pair(src, x, dst, w);
                    }
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                    for (x = clip_w1 << 1, w = clip_w1; w < words; w += 2, x += 4) {
                        blur_block_pair(src, x, dst, w);
                    }
                } else {
                    uint32_t words = active_px >> 1;
                    for (uint32_t x = 0, w = 0; w < words; w += 2, x += 4) {
                        blur_block_pair(src, x, dst, w);
                    }
                }
            } else {
                // Word-by-word copy instead of one memcpy burst: a burst read
                // "photographs" the row at a single instant, which easily lands
                // mid-write (the hard tear). Reading word by word spreads the
                // loads across the line so they more often land on already
                // written, final data. Same bytes, same result - only the read
                // pattern changes.
                const uint16_t *src = &g_framebuf[FRAME_WIDTH * r0];
                for (uint32_t w = 0; w < FRAME_WIDTH / 2; w++) {
                    dst[w] = load16x2(src + (w << 1));
                }
                if (has_clip) {
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                }
            }
        } else {
            // 240P (and 720P): 2x horizontal repeat of the 640 captured
            // samples. Vertical: 240P is 1:1, 720P is 3:1 (src_row above).
            int32_t r0i = (int32_t)src_row + vy;
            if (r0i < 0 || r0i >= (int32_t)FRAME_HEIGHT) {
                // Whole line off the source: solid black (OSD box still on top).
                memset(dst, 0, (active_px >> 1) * sizeof(uint32_t));
                if (has_clip) {
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                }
                return;
            }
            const uint16_t *src = &g_framebuf[FRAME_WIDTH * (uint32_t)r0i];
            if (blur) {
                // Horizontal softening: each output pair shows the average of
                // sample x and its right neighbour.
                if (has_clip) {
                    // Skip the OSD box words [clip_w0, clip_w1) (in 240P the
                    // word index IS the source pixel column) and blit the
                    // pre-rendered box row in the MIDDLE of the line, mirroring
                    // the 480P paths: writing the box at the very end pushed
                    // the whole-line transform against the scanline IRQ budget
                    // and the HSTX underflowed, dropping the box. Skipping the
                    // box also shrinks the fill cost.
                    for (uint32_t x = 0; x < clip_w0; x++) {
                        uint32_t nx = x + 1;
                        if (nx >= FRAME_WIDTH) nx = x;
                        uint32_t q = blend16(src[x], src[nx]);
                        dst[x] = q | (q << 16);
                    }
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                    for (uint32_t x = clip_w1; x < FRAME_WIDTH; x++) {
                        uint32_t nx = x + 1;
                        if (nx >= FRAME_WIDTH) nx = x;
                        uint32_t q = blend16(src[x], src[nx]);
                        dst[x] = q | (q << 16);
                    }
                } else {
                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t nx = x + 1;
                        if (nx >= FRAME_WIDTH) nx = x;
                        uint32_t q = blend16(src[x], src[nx]);
                        dst[x] = q | (q << 16);
                    }
                }
            } else {
                if (has_clip) {
                    for (uint32_t x = 0; x < clip_w0; x++) {
                        uint32_t p = src[x];
                        dst[x] = p | (p << 16);
                    }
                    osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                    for (uint32_t x = clip_w1; x < FRAME_WIDTH; x++) {
                        uint32_t p = src[x];
                        dst[x] = p | (p << 16);
                    }
                } else {
                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t p = src[x];
                        dst[x] = p | (p << 16);
                    }
                }
            }
        }
        return;
    }

    // General viewport path: fixed-point 16.16 stepping. dst is uint32 words,
    // one word = two output pixels, so sample two viewport pixels and pack
    // them per word (mirrors the 240P fast-path layout above; writing one
    // uint16 per word here would overflow the line buffer 2x). Signed: samples
    // outside the source are black.
    //
    // Per-word cost matters: this runs every scanline, and at 480P the whole
    // line has ~8000 cycles at 252 MHz. An overrun lets the HSTX line buffer
    // underflow, which is what made the sink drop the signal when zooming in
    // 480P. So the common in-frame case gets a specialized loop with NO
    // per-sample bounds checks; only the off-edge case (pan past the edge,
    // or zoom-out where the picture sits inside a black frame) pays for them.
    // The blur keeps to that budget by using the same block-based filter as
    // the zoom=100 fast path (one source pair + intra-word average per word).
    int32_t row_src = vy + (int32_t)((active_line * vh) / active_lines);
    if (row_src < 0 || row_src >= (int32_t)FRAME_HEIGHT) {
        // The whole line is off the source: solid black (the OSD box still
        // shows on top).
        memset(dst, 0, (active_px >> 1) * sizeof(uint32_t));
        if (has_clip) {
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        }
        return;
    }
    const uint16_t *src = &g_framebuf[FRAME_WIDTH * (uint32_t)row_src];
    uint32_t words = active_px >> 1;

    if (active_px != FRAME_WIDTH) {
        // 1280-wide (240P/720P): one source sample per output word (the 2x
        // repeat of the 640-sample capture), stepped per word. The 480P path
        // below samples two source pixels per word; doing that here doubled
        // the transform cost at 720P (HSTX underflow -> sink dropped the
        // signal when changing Zoom/Pos/Blur) and shifted the sampled window
        // half a pixel right of the fast-path layout. See
        // scanline_general_fill_wide.
        uint32_t wstep = ((uint32_t)vw << 16) / words;
        if (vx >= 0 && vx + (int32_t)vw <= (int32_t)FRAME_WIDTH) {
            // The whole sampled window is inside the source: no bounds checks.
            int32_t acc = (int32_t)((uint32_t)vx << 16);
            if (!has_clip) {
                scanline_general_fill_wide(src, dst, acc, wstep, 0, words, blur);
            } else {
                scanline_general_fill_wide(src, dst, acc, wstep, 0, clip_w0, blur);
                osd_box_blit(dst, clip_w0, clip_w1, clip_src);
                int32_t acc2 = acc + (int32_t)(clip_w1 * wstep);
                scanline_general_fill_wide(src, dst, acc2, wstep, clip_w1, words, blur);
            }
            return;
        }
        // The viewport reaches past the source edge: samples are monotonic, so
        // only the edges can fall off. Split into a black prefix, an unchecked
        // interior and a black suffix (same shape as the 480P path below).
        int32_t acc0 = (int32_t)((uint32_t)vx << 16);
        uint32_t w_lo = 0;
        if (acc0 < 0) {
            w_lo = (uint32_t)(((uint32_t)(-acc0) + wstep - 1) / wstep);
        }
        int32_t hi = ((int32_t)FRAME_WIDTH << 16) - acc0 - 1;
        uint32_t w_hi = (hi > 0) ? (uint32_t)(((uint32_t)hi + wstep - 1) / wstep) : 0;
        if (w_lo > words) w_lo = words;
        if (w_hi > words) w_hi = words;
        if (w_lo >= w_hi) {
            memset(dst, 0, words * sizeof(uint32_t));
            if (has_clip) {
                osd_box_blit(dst, clip_w0, clip_w1, clip_src);
            }
            return;
        }
        if (w_lo != 0) {
            memset(dst, 0, w_lo * sizeof(uint32_t));
        }
        int32_t acc = acc0 + (int32_t)(w_lo * wstep);
        if (!has_clip) {
            scanline_general_fill_wide(src, dst, acc, wstep, w_lo, w_hi, blur);
        } else {
            // Skip the OSD box region [clip_w0, clip_w1) inside the interior
            // and blit it in the MIDDLE of the line (see the 480P path below).
            uint32_t a1 = (clip_w0 > w_lo) ? clip_w0 : w_lo;
            if (a1 > w_hi) a1 = w_hi;
            uint32_t b0 = (clip_w1 < w_hi) ? clip_w1 : w_hi;
            if (b0 < w_lo) b0 = w_lo;
            scanline_general_fill_wide(src, dst, acc, wstep, w_lo, a1, blur);
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
            int32_t acc2 = acc + (int32_t)((b0 - w_lo) * wstep);
            scanline_general_fill_wide(src, dst, acc2, wstep, b0, w_hi, blur);
        }
        if (w_hi < words) {
            memset(&dst[w_hi], 0, (words - w_hi) * sizeof(uint32_t));
        }
        return;
    }

    if (blur) {
        // Block-based horizontal blur mapped through the viewport: each output
        // word shows the average of one source pixel pair at the viewport
        // position, exactly the filter the zoom=100 fast path uses (so the
        // look matches). One source pair per word means ~1 load + the 3-op
        // intra-word average, about the same cost as plain sampling, so
        // zooming/panning with blur does not overrun the scanline IRQ.
        // Blocks that fall off the source edge are black.
        uint32_t sstep = ((uint32_t)vw << 17) / active_px;
        int32_t sacc0 = (int32_t)((uint32_t)vx << 16);
        // s advances monotonically, so only the edges can fall off the source:
        // split the line into a black prefix, an unchecked interior and a black
        // suffix. The old per-word bounds-checked loop cost ~2x a plain line
        // and overran the ~5k-cycle scanline IRQ budget whenever the viewport
        // reached past the source edge (zoom-out / pan), which made the sink
        // drop the signal. The split adds two 32-bit divides and two memsets.
        uint32_t w_lo = 0;
        if (sacc0 < 0) {
            w_lo = (uint32_t)(((uint32_t)(-sacc0) + sstep - 1) / sstep);
        }
        int32_t hi = ((int32_t)(FRAME_WIDTH - 1) << 16) - sacc0;
        uint32_t w_hi = (hi > 0) ? (uint32_t)(((uint32_t)hi + sstep - 1) / sstep) : 0;
        if (w_lo > words) w_lo = words;
        if (w_hi > words) w_hi = words;
        if (w_lo >= w_hi) {
            memset(dst, 0, words * sizeof(uint32_t));
            if (has_clip) {
                osd_box_blit(dst, clip_w0, clip_w1, clip_src);
            }
            return;
        }
        memset(dst, 0, w_lo * sizeof(uint32_t));
        memset(&dst[w_hi], 0, (words - w_hi) * sizeof(uint32_t));
        if (!has_clip) {
            int32_t sacc = sacc0 + (int32_t)(w_lo * sstep);
            uint32_t *o = &dst[w_lo];
            uint32_t n = w_hi - w_lo;
            for (uint32_t w = 0; w < n; w++) {
                int32_t s = sacc >> 16;
                sacc += sstep;
                uint32_t wrd = load16x2(&src[s]);
                uint32_t q = ((wrd >> 1) & 0x7BEFu) + ((wrd >> 17) & 0x7BEFu);
                o[w] = q | (q << 16);
            }
        } else {
            // Skip the OSD box region [clip_w0, clip_w1) inside the interior
            // and blit it in the MIDDLE of the line instead of at the end:
            // writing the box early keeps the HSTX DMA for the box safe even if
            // this line's fill is marginal against the scanline IRQ budget, and
            // skipping the box also shrinks the fill cost (~10 cycles per word).
            uint32_t a1 = (clip_w0 > w_lo) ? clip_w0 : w_lo;
            if (a1 > w_hi) a1 = w_hi;
            uint32_t b0 = (clip_w1 < w_hi) ? clip_w1 : w_hi;
            if (b0 < w_lo) b0 = w_lo;
            int32_t sacc = sacc0 + (int32_t)(w_lo * sstep);
            for (uint32_t w = w_lo; w < a1; w++) {
                int32_t s = sacc >> 16;
                sacc += sstep;
                uint32_t wrd = load16x2(&src[s]);
                uint32_t q = ((wrd >> 1) & 0x7BEFu) + ((wrd >> 17) & 0x7BEFu);
                dst[w] = q | (q << 16);
            }
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
            sacc = sacc0 + (int32_t)(b0 * sstep);
            for (uint32_t w = b0; w < w_hi; w++) {
                int32_t s = sacc >> 16;
                sacc += sstep;
                uint32_t wrd = load16x2(&src[s]);
                uint32_t q = ((wrd >> 1) & 0x7BEFu) + ((wrd >> 17) & 0x7BEFu);
                dst[w] = q | (q << 16);
            }
        }
        return;
    }

    uint32_t step = ((uint32_t)vw << 16) / active_px;

    if (vx >= 0 && vx + (int32_t)vw <= (int32_t)FRAME_WIDTH) {
        // The whole sampled window is inside the source: no bounds checks.
        int32_t acc = (int32_t)((uint32_t)vx << 16);
        if (!has_clip) {
            scanline_general_fill(src, dst, acc, step, 0, words);
        } else {
            scanline_general_fill(src, dst, acc, step, 0, clip_w0);
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
            int32_t acc2 = acc + (int32_t)(clip_w1 * (step << 1));
            scanline_general_fill(src, dst, acc2, step, clip_w1, words);
        }
        return;
    }

    // The viewport reaches past the source edge: the samples are monotonic, so
    // only the edges can fall off. Split the line into a black prefix, an
    // unchecked interior and a black suffix instead of per-word bounds checks.
    // The old checked loop cost ~2x a plain line and overran the ~5k-cycle
    // scanline IRQ budget (zoom-out / pan past the edge), underflowing the
    // HSTX FIFO and making the sink drop the signal.
    uint32_t dstep = step << 1;
    int32_t acc0 = (int32_t)((uint32_t)vx << 16);
    uint32_t w_lo = 0;
    if (acc0 < 0) {
        w_lo = (uint32_t)(((uint32_t)(-acc0) + dstep - 1) / dstep);
    }
    int32_t hi = ((int32_t)FRAME_WIDTH << 16) - acc0 - (int32_t)step;
    uint32_t w_hi = (hi > 0) ? (uint32_t)(((uint32_t)hi + dstep - 1) / dstep) : 0;
    if (w_lo > words) w_lo = words;
    if (w_hi > words) w_hi = words;
    if (w_lo >= w_hi) {
        memset(dst, 0, words * sizeof(uint32_t));
        if (has_clip) {
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        }
        return;
    }
    if (w_lo != 0) {
        memset(dst, 0, w_lo * sizeof(uint32_t));
    }
    int32_t acc = acc0 + (int32_t)(w_lo * dstep);
    if (!has_clip) {
        scanline_general_fill(src, dst, acc, step, w_lo, w_hi);
    } else {
        // Skip the OSD box region [clip_w0, clip_w1) inside the interior and
        // blit it instead (it may also cover black edges - clamp each fill
        // run to the interior [w_lo, w_hi) and let the unconditional blit
        // overwrite the box rectangle).
        uint32_t a1 = (clip_w0 > w_lo) ? clip_w0 : w_lo;
        if (a1 > w_hi) a1 = w_hi;
        uint32_t b0 = (clip_w1 < w_hi) ? clip_w1 : w_hi;
        if (b0 < w_lo) b0 = w_lo;
        scanline_general_fill(src, dst, acc, step, w_lo, a1);
        osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        int32_t acc2 = acc + (int32_t)((b0 - w_lo) * dstep);
        scanline_general_fill(src, dst, acc2, step, b0, w_hi);
    }
    if (w_hi < words) {
        memset(&dst[w_hi], 0, (words - w_hi) * sizeof(uint32_t));
    }
}

// ----------------------------------------------------------------------------
// 4:3 ("square") mode at 720P: the 640-sample capture is displayed in a
// centered 960px-wide (480-word) region with 160px (80-word) black bars on
// each side, preserving the N64's native 4:3 proportions (the 16:9 720P
// fill stretches the same capture across the full 1280px). Horizontal
// downscale 640 -> 480 words via fixed-point 16.16 stepping (stride 4/3),
// the same mapping as apps/n64_diag_720p. The OSD box (in 720P word
// coordinates) is skipped in the fill and blitted over the center, like the
// other paths. Vertical scaling stays 3:1 (active_line / 3).
//
// zoom/pos apply exactly as in the 16:9 path: the viewport (vw x vh samples
// at offset vx, vy) is mapped into the fixed 480-word x 720-line region, so
// zoom in magnifies a window of the capture, zoom out shows more of it, and
// pan slides the window (black where the viewport leaves the source). The
// sampling reuses scanline_general_fill_wide() (one source sample per output
// word + optional blur), keeping this within the pipelined 720P budget.
// ----------------------------------------------------------------------------
#define SQUARE_WIDTH_WORDS (FRAME_WIDTH * 3 / 4)                       // 480 words = 960 px
#define SQUARE_BAR_WORDS   ((FRAME_WIDTH - SQUARE_WIDTH_WORDS) / 2)    // 80 words = 160 px

static void scanline_map_square(uint32_t active_line, uint32_t *dst,
                                uint32_t clip_w0, uint32_t clip_w1,
                                const uint32_t *clip_src)
{
    const bool has_clip = (clip_src != NULL) && (clip_w0 < clip_w1);
    const bool blur = g_config.blur_enabled;

    // Viewport (identical clamp logic to scanline_map()).
    uint32_t zoom = g_config.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 400) zoom = 400;
    uint32_t vw = (FRAME_WIDTH * 100u) / zoom;
    uint32_t vh = (FRAME_HEIGHT * 100u) / zoom;
    if (vw == 0) vw = 1;
    if (vh == 0) vh = 1;
    int32_t vx = ((int32_t)FRAME_WIDTH - (int32_t)vw) / 2 + g_config.pos_x;
    // Same menu-open pan as scanline_map(): the GAME (not the box) is panned
    // down while the menu is open, per output mode.
    int32_t vy = ((int32_t)FRAME_HEIGHT - (int32_t)vh) / 2
        + g_config.pos_y - (osd_is_open()
            ? (int32_t)((video_output_active_mode->v_active_lines == 720U)
                        ? OSD_OPEN_Y_SHIFT_720P : OSD_OPEN_Y_SHIFT)
            : 0);
    if (vx < -(int32_t)vw) vx = -(int32_t)vw;
    if (vx > (int32_t)FRAME_WIDTH) vx = FRAME_WIDTH;
    if (vy < -(int32_t)vh) vy = -(int32_t)vh;
    if (vy > (int32_t)FRAME_HEIGHT) vy = FRAME_HEIGHT;

    const uint32_t active_lines = video_output_active_mode->v_active_lines;

    // Whole line off the source vertically: solid black (OSD box still on top).
    int32_t row_src = vy + (int32_t)((active_line * vh) / active_lines);
    if (row_src < 0 || row_src >= (int32_t)FRAME_HEIGHT) {
        memset(dst, 0, (video_output_active_mode->h_active_pixels >> 1) * sizeof(uint32_t));
        if (has_clip) {
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        }
        return;
    }
    const uint16_t *src = &g_framebuf[FRAME_WIDTH * (uint32_t)row_src];

    // Black bars, 80 words (160 px) on each side.
    memset(dst, 0, SQUARE_BAR_WORDS * sizeof(uint32_t));
    memset(&dst[SQUARE_BAR_WORDS + SQUARE_WIDTH_WORDS], 0,
           SQUARE_BAR_WORDS * sizeof(uint32_t));

    // The picture region [SQUARE_BAR_WORDS, SQUARE_BAR_WORDS + SQUARE_WIDTH_WORDS).
    // The OSD box sits inside it (both are centered), so clamp the box clip to
    // the region and skip it mid-fill.
    uint32_t a0 = SQUARE_BAR_WORDS;
    uint32_t b0 = SQUARE_BAR_WORDS + SQUARE_WIDTH_WORDS;
    if (has_clip) {
        if (clip_w0 < a0) clip_w0 = a0;
        if (clip_w1 > b0) clip_w1 = b0;
    }

    // Viewport width over the 480 words (1 source sample per output word).
    uint32_t wstep = ((uint32_t)vw << 16) / SQUARE_WIDTH_WORDS;
    int32_t acc0 = (int32_t)((uint32_t)vx << 16);

    // Samples are monotonic, so only the edges can fall off the source: split
    // the picture region into black edges, an unchecked interior and black
    // edges (same shape as scanline_map()'s off-edge handling).
    uint32_t w_lo = a0;
    uint32_t w_hi = b0;
    if (acc0 < 0) {
        w_lo = a0 + (uint32_t)(((uint32_t)(-acc0) + wstep - 1) / wstep);
    }
    int32_t hi = ((int32_t)FRAME_WIDTH << 16) - acc0 - 1;
    uint32_t n_hi = (hi > 0) ? (uint32_t)(((uint32_t)hi + wstep - 1) / wstep) : 0;
    if (n_hi < SQUARE_WIDTH_WORDS) {
        w_hi = a0 + n_hi;
    }
    if (w_lo > b0) w_lo = b0;
    if (w_hi > b0) w_hi = b0;
    if (w_lo >= w_hi) {
        // Nothing in-frame: the picture region is solid black.
        memset(&dst[a0], 0, SQUARE_WIDTH_WORDS * sizeof(uint32_t));
        if (has_clip) {
            osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        }
        return;
    }
    if (w_lo != a0) {
        memset(&dst[a0], 0, (w_lo - a0) * sizeof(uint32_t));
    }
    if (w_hi != b0) {
        memset(&dst[w_hi], 0, (b0 - w_hi) * sizeof(uint32_t));
    }
    int32_t acc = acc0 + (int32_t)((w_lo - a0) * wstep);
    if (!has_clip) {
        scanline_general_fill_wide(src, dst, acc, wstep, w_lo, w_hi, blur);
    } else {
        // Skip the OSD box region [clip_w0, clip_w1) inside the interior and
        // blit it in the MIDDLE of the line (see the 480P path).
        uint32_t a1 = (clip_w0 > w_lo) ? clip_w0 : w_lo;
        if (a1 > w_hi) a1 = w_hi;
        uint32_t b1 = (clip_w1 < w_hi) ? clip_w1 : w_hi;
        if (b1 < w_lo) b1 = w_lo;
        scanline_general_fill_wide(src, dst, acc, wstep, w_lo, a1, blur);
        osd_box_blit(dst, clip_w0, clip_w1, clip_src);
        int32_t acc2 = acc + (int32_t)((b1 - w_lo) * wstep);
        scanline_general_fill_wide(src, dst, acc2, wstep, b1, w_hi, blur);
    }
}

// ----------------------------------------------------------------------------
// OSD overlay mode (test build): while the menu is open the capture loop
// keeps running, so scanline_map() fills every line with LIVE video. The
// menu band is drawn by scanline_map() itself in BOTH 480P and 240P: it
// skips the OSD box rectangle in the transform and blits the pre-rendered
// box row (g_osd_box_buf, 480P layout; osd_box_blit() expands it for 240P)
// over it in the middle of the line, so the OSD stays on top of the
// transform everywhere without pushing the fill past the IRQ budget.
// ----------------------------------------------------------------------------
#ifdef N64_HDMI_OSD_OVERLAY
// Cheap 1:1 fill for the menu-band lines (see scanline_callback). Running the
// zoom/pan/blur transform AND the OSD paint on those lines overran the scanline
// IRQ budget (transform ~2.6-3.8k cycles + paint ~1.2k at 480P; the effective
// budget is ~4k), which underflowed the HSTX FIFO - the sink dropped the
// signal (zoom>100 / pan) or the menu vanished (blur / zoom<100). The band
// stays 1:1: the solid menu box covers the center anyway, and the live
// zoom/pan/blur preview is visible on the game lines above and below.
static void scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    const video_mode_t *mode = video_output_active_mode;
    const uint32_t src_row = (mode->h_active_pixels == FRAME_WIDTH)
        ? (active_line >> 1)
        : ((mode->v_active_lines == 720U) ? (active_line / 3) : active_line);
    // 4:3 with side bars only exists at 720P (480P/240P ignore it).
    const bool square = (mode->v_active_lines == 720U) && g_config.aspect_ratio;
    const bool menu_open = osd_is_open();
    // While the menu is open the GAME viewport is shifted OSD_OPEN_Y_SHIFT
    // source rows DOWN via Pos Y (scanline_map()/scanline_map_square() add the
    // offset to vy): opening the menu makes the game appear to jump a few rows
    // (the capture and HDMI output share one framebuffer and are not
    // phase-locked), so panning the game down puts it back while the box band
    // stays put (the "Pos Y" item still shows the raw config value, 0).
    if (menu_open && g_osd_span_rows != 0 &&
        src_row >= OSD_MENU_TOP_ROW && src_row < (OSD_MENU_TOP_ROW + g_osd_span_rows)) {
        // Menu line. The transform must run across the WHOLE width (no 1:1
        // band), but its expensive general paths skip the OSD box rectangle
        // and scanline_map() blits the pre-rendered box row over it: transform
        // + blit stays inside the IRQ budget at 480P (box in output words is
        // [box_x0/2, box_x1/2)) and at 240P (box in output words is
        // [box_x0, box_x1), word = source pixel; osd_box_blit() expands the
        // 480P-layout row). 720P uses the same [box_x0, box_x1) word coords.
        const uint32_t item_row = src_row - OSD_MENU_TOP_ROW;
        if (square) {
            scanline_map_square(active_line, dst,
                                g_osd_box_x0, (uint32_t)g_osd_box_x0 + g_osd_box_w,
                                g_osd_box_buf[item_row]);
        } else if (mode->h_active_pixels == FRAME_WIDTH) {
            scanline_map(active_line, dst,
                         g_osd_box_x0 >> 1, (g_osd_box_x0 + g_osd_box_w) >> 1,
                         g_osd_box_buf[item_row]);
        } else {
            scanline_map(active_line, dst,
                         g_osd_box_x0, (uint32_t)g_osd_box_x0 + g_osd_box_w,
                         g_osd_box_buf[item_row]);
        }
        return;
    }
    if (square) {
        scanline_map_square(active_line, dst, 0, 0, NULL);
    } else {
        scanline_map(active_line, dst, 0, 0, NULL);
    }
}
#else
static void scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    scanline_map(active_line, dst, 0, 0, NULL);
}
#endif

// ----------------------------------------------------------------------------
// Boot combo recovery: a C-Up + one-C-button chord held at power-on forces a
// clean boot (defaults, no saved overrides) into a chosen output mode, as a
// case-less alternative to BOOTSEL:
//   C-Up + C-Right -> 720P (output_mode 2)
//   C-Up + C-Down  -> 480P (output_mode 1)
//   C-Up + C-Left  -> 240P (output_mode 0)
//
// The N64's PIF is busy with the cartridge CIC handshake during IPL and only
// starts polling the controller once the game calls osContInit(), so the
// combo CANNOT be read during the Pico's boot-time init. Instead the check
// is armed for the first seconds of the capture loop (while the game is
// loading) and, on BOOT_COMBO_REQUIRED_READS consecutive polls showing a
// chord, the saved config is reset to defaults, forced to the chord's mode,
// saved, and the Pico reboots - the next boot comes up clean in that mode.
// It works even when the saved mode leaves the display black (the capture
// loop runs regardless of the HDMI output).
//
// "Already that mode" ignore: if the chord's target equals the mode that was
// just booted, there is nothing to recover from, so the chord is simply
// ignored (no reboot) - e.g. holding C-Up+C-Right while already booted at
// 720P does nothing.
//
// One-shot latch: the N64 keeps running and polling the controller through
// the Pico's reboot, so on the recovery boot the user may still be holding
// the chord. g_config.boot_combo_fired makes that boot return early instead
// of re-arming; the latch is cleared once a non-chord poll is seen (the user
// let go), re-arming future boots.
//
// Safety: the window only lasts a few seconds, a non-chord poll disarms it
// immediately (the game is running and the user isn't holding a chord), and
// the OSD menu being open also suppresses it, so normal play can never trip
// it.
// ----------------------------------------------------------------------------
#define BOOT_COMBO_WINDOW_US      (15 * 1000000u) // armed window after capture start
#define BOOT_COMBO_REQUIRED_READS 3               // consecutive polls with the chord

// Map a C-Up + one-C-button chord to its target output_mode (0/1/2), or -1
// if no boot combo is being pressed.
static int boot_combo_target(uint32_t buttons)
{
    if (CU_BUTTON(buttons)) {
        if (CR_BUTTON(buttons)) return 2; // C-Up + C-Right -> 720P
        if (CD_BUTTON(buttons)) return 1; // C-Up + C-Down  -> 480P
        if (CL_BUTTON(buttons)) return 0; // C-Up + C-Left  -> 240P
    }
    return -1;
}

static const char *boot_combo_mode_name(int mode)
{
    return mode == 2 ? "720P" : mode == 1 ? "480P" : "240P";
}

static void check_boot_combo(void)
{
    // Recovery boot after a chord-fired reset: the user may still be holding
    // the chord, so never re-arm this session. Clear the latch once a
    // non-chord poll shows the buttons were released (future boots re-arm).
    if (g_config.boot_combo_fired == 1) {
        if (joybus_rx_saw_data()) {
            uint32_t buttons = joybus_rx_get_latest();
            if (boot_combo_target(buttons) < 0) {
                g_config.boot_combo_fired = 0;
                config_save();
            }
        }
        return;
    }

    static bool done = false;
    static bool armed = false;
    static uint32_t start_us;
    static uint32_t count = 0;
    static int target = -1;

    if (done) {
        return;
    }
    if (!armed) {
        armed = true;
        start_us = time_us_32();
    } else if ((time_us_32() - start_us) > BOOT_COMBO_WINDOW_US) {
        done = true; // window expired: normal boot
        return;
    }

    if (osd_is_open()) {
        return; // don't trip while navigating the menu
    }

    uint32_t buttons = joybus_rx_get_latest();
    int cur = boot_combo_target(buttons);

    if (cur < 0) {
        // No chord held. Once any real poll has arrived (the game is running
        // and the user isn't holding a chord) disarm immediately; before that
        // we just keep waiting for the first poll or the window timeout.
        if (joybus_rx_saw_data()) {
            done = true;
        }
        count = 0;
        target = -1;
        return;
    }

    // "Already that mode": the held chord targets the mode that was just
    // booted, so there is nothing to recover from - ignore it.
    if (cur == (int)g_config.output_mode) {
        done = true;
        return;
    }

    if (cur != target) {
        target = cur;
        count = 0;
    }

    if (++count >= BOOT_COMBO_REQUIRED_READS) {
        done = true;
        printf("Boot combo: resetting to clean %s defaults\n", boot_combo_mode_name(cur));
        config_init();                // back to compile-time defaults
        g_config.output_mode = (uint32_t)cur;
        g_config.boot_combo_fired = 1; // latch: don't re-fire on the recovery boot
        config_save();
        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------
int main(void)
{
    config_init();

    // BOOTSEL held at power-on = factory reset: ignore the saved config
    // and (once stdio is up) erase it, so the build's default mode comes
    // back. This is the recovery path if a saved config leaves the output
    // with no signal: the config sector lives in the last flash sector,
    // which normal UF2 flashing never touches, so this button is the only
    // thing that can clear a bad boot mode without re-flashing.
    bool factory_reset = bootsel_button_is_pressed();
    if (!factory_reset) {
        config_load();
    }
    // Else: keep the compile-time defaults from config_init().

    // Clock/voltage setup differs by output mode, matching the three
    // independently-confirmed-working configs on this hardware:
    //   - 240p (output_mode 0): stock 126 MHz sys_clk, default core
    //     voltage, no ramp - confirmed via the vendored directvideo_240p
    //     reference example (works on 2 of 3 displays tested; the third
    //     likely just doesn't support true 240p over HDMI, a common display
    //     limitation). With N64_HDMI_240P_OC252 the CPU instead runs at
    //     252 MHz (see below).
    //   - 480p (output_mode 1): 252 MHz sys_clk / VREG 1.15V, executing
    //     from RAM (pico_set_binary_type ... copy_to_ram, set in
    //     CMakeLists.txt) - confirmed via a standalone hstx_test_simple
    //     built against this same vendored pico_hdmi.
    //     n64_hdmi_480p_confirmed's hstx_clk_div=2 keeps the actual pixel
    //     clock at 25.2 MHz despite the doubled sys_clk, so the signal
    //     timing is the same either way - only the CPU's own clock margin
    //     differs.
    //   - 720p (output_mode 2): 372 MHz sys_clk / VREG 1.30V, executing
    //     from RAM (video_mode_720_p_cea, div=1 -> 74.4 MHz pixel clock) -
    //     same clock/voltage sequence as apps/n64_diag_720p / bouncing_box_rt.
    uint32_t boot_mode_sel = g_config.output_mode;
    if (boot_mode_sel > 2) boot_mode_sel = 0;
#if defined(N64_HDMI_240P_OC252)
    // Diagnostic/fix build: 240p but with the CPU at 252 MHz (the
    // n64_hdmi_240p_confirmed descriptor keeps the pixel clock at 25.2 MHz
    // via hstx_clk_div=2, so the wire signal is identical to the stock-126 MHz
    // 240p build -- only the CPU margin changes). The stock 240p config loses
    // capture FIFO headroom at 126 MHz (the 565 capture loop needs ~9.3k
    // cycles/line vs ~6k available), which shows up as "garbage with the
    // game's colors".
    const bool oc252 = true;
#else
    const bool oc252 = false;
#endif
    if (boot_mode_sel == 1) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        sleep_ms(10);
        set_sys_clock_khz(252000, true);
    } else if (boot_mode_sel == 2) {
        vreg_set_voltage(VREG_VOLTAGE_1_30);
        sleep_ms(10);
        set_sys_clock_khz(372000, true);
    } else if (oc252) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        sleep_ms(10);
        set_sys_clock_khz(252000, true);
    } else {
        set_sys_clock_khz(126000, true);
    }

    stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
    sleep_ms(1000); // matches pico_hdmi's own examples; give the display a moment before HDMI init

    if (factory_reset) {
        // Erase the saved config so the reset is permanent across reboots
        // (done here, after stdio init, so it can be logged; still before
        // any PIO/DMA/Core1 start, so the flash access is safe).
        printf("BOOTSEL held at boot: factory reset, erasing saved config\n");
        config_erase();
        printf("Factory reset complete, using defaults\n");
    }

    printf("Configuring HDMI output (pico_hdmi / HSTX)\n");

    gfx_init();

#ifdef DIAGNOSTICS
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
        g_framebuf[i] = RGB888_TO_RGB565(0xFF, 0x00, 0x00);
    }
#else
    memset(g_framebuf, 0x00, sizeof(g_framebuf));
#endif

#ifdef N64_HDMI_AUDIO_ONLY
    // 8 vertical bars across the 640 sample-wide output.
    const uint16_t bar_colors[8] = {
        RGB888_TO_RGB565(0xE8, 0x00, 0x00), RGB888_TO_RGB565(0xE8, 0xA0, 0x00),
        RGB888_TO_RGB565(0x00, 0xE8, 0x00), RGB888_TO_RGB565(0x00, 0xE8, 0xE8),
        RGB888_TO_RGB565(0x00, 0x00, 0xE8), RGB888_TO_RGB565(0xE8, 0x00, 0xE8),
        RGB888_TO_RGB565(0xE8, 0xE8, 0xE8), RGB888_TO_RGB565(0x50, 0x50, 0x50),
    };
    for (int i = 0; i < FRAME_WIDTH; i++) {
        test_pattern[i] = bar_colors[i / (FRAME_WIDTH / 8)];
    }
#endif

    // Apply the build-time HSTX pinout preset (CMake N64_HDMI_PINOUT). For
    // the pico_hdmi preset the library's built-in default already matches
    // this board, so no call is needed - see the comment near the top of
    // this file. The dvisock preset re-routes the TMDS pairs for the
    // Pimoroni Pico DVI Sock. Must run before video_output_core1_run().
#ifdef N64_HDMI_PINOUT_DVI_SOCK
    if (!video_output_set_hstx_pinout(&dvi_sock_pinout)) {
        printf("ERROR: failed to set dvisock HSTX pinout\n");
    }
#endif
    hstx_di_queue_init();

    // Runtime API (video_output_rt.c / video_output_set_mode()) instead of
    // the compile-time video_output.c backend: on this hardware, the
    // compile-time path produced audio but no picture on every display
    // tested (bouncing_box, hstx_test_default/custom, n64_diag_noskip all
    // affected identically), while the runtime path's own examples
    // (bouncing_box_rt, directvideo_240p) showed a clean picture. See the
    // README section "Nuevo dato: la API runtime funciona, la classic no"
    // for the empirical trail. video_output_set_mode() must be called
    // before video_output_init().
    //
    // Boot mode selection: pick the mode descriptor matching the saved
    // output_mode, so the OSD's "Video Mode" change survives a reboot via
    // config_save(). The sys_clk was already set to match above (126 / 252 /
    // 372 MHz), so each descriptor keeps a spec-legal pixel clock.
    const video_mode_t *boot_mode;
    switch (boot_mode_sel) {
    case 2:
        boot_mode = &video_mode_720_p_cea;
        break;
    case 1:
        boot_mode = &n64_hdmi_480p_confirmed;
        break;
#if defined(N64_HDMI_240P_OC252)
    case 0:
        // 240p at 252 MHz sys_clk: the custom div=2 descriptor keeps the
        // pixel clock at 25.2 MHz (the library's stock video_mode_240_p is
        // div=1 and would double it to 50.4 MHz at this clock).
        boot_mode = &n64_hdmi_240p_confirmed;
        break;
#else
    default:
        boot_mode = &video_mode_240_p;
        break;
#endif
    }
    video_output_set_mode(boot_mode);
    video_output_init(boot_mode->h_active_pixels, boot_mode->v_active_lines);

#if AUDIO_ENABLED
    pico_hdmi_set_audio_sample_rate(g_config.audio_out_sample_rate);
#endif
    // Never call video_output_set_dvi_mode(true): this board needs HDMI
    // mode (data islands) for a stable picture - DVI-only mode was found
    // to produce no-video/instability on this hardware (see README), and
    // n64_diag_noskip (stable) doesn't call it either. The library's
    // default is HDMI mode, so this is also just the least code.

    video_output_set_scanline_callback(scanline_callback);
#if AUDIO_ENABLED
    video_output_set_background_task(hdmi_audio_task);
#endif

    printf("Core 1 start (HDMI/HSTX output)\n");
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    printf("Start capture\n");

    // One-time setup checkpoints (safe: this all happens strictly BEFORE
    // the capture while(1) loop starts, so unlike a printf() inside the
    // loop - confirmed on real hardware to cause visible field drops in
    // n64_diag_noskip - none of these can disturb PIO/HDMI timing. If
    // n64_hdmi ever fails to show a picture at all (not even black, i.e.
    // video never even starts, as opposed to N64 capture issues once
    // video is already up), the last checkpoint printed tells you exactly
    // which setup step it didn't get past.
    printf("checkpoint: gpio pins\n");

    // N64 digital video/audio input pins + Joybus pin: same setup as the
    // original code, entirely unaffected by the output backend swap.
    for (int i = PIN_VIDEO_D0; i <= PIN_AUDIO_BCLK; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);

        // Enable weak internal pull downs to reduce noise when N64 is turned off
        gpio_set_pulls(i, false, true);
    }

    gpio_init(PIN_JOYBUS_P1);
    gpio_set_dir(PIN_JOYBUS_P1, GPIO_IN);
    gpio_set_pulls(PIN_JOYBUS_P1, false, false);

    printf("checkpoint: video PIO\n");

    uint offset = pio_add_program(pio_n64, &n64_program);
#if !defined(N64_HDMI_AUDIO_ONLY)
    // Video capture PIO program (unchanged)
    n64_video_program_init(pio_n64, sm_video, offset);
    pio_sm_set_enabled(pio_n64, sm_video, true);
#endif

    printf("checkpoint: audio PIO\n");

#if AUDIO_ENABLED
#if !defined(N64_HDMI_AUDIO_SILENT) || defined(N64_HDMI_AUDIO_SILENT_SM1)
    // Audio capture PIO program (unchanged). The SILENT_SM1 variant keeps
    // SM1 running on PIO1 (stealing round-robin slots from the video SM0)
    // while the task pushes silence and no DMA drains SM1 -- isolates
    // whether SM1's mere presence on PIO1 is what breaks the picture.
    n64_audio_program_init(pio_n64, sm_audio, offset);
    pio_sm_set_enabled(pio_n64, sm_audio, true);
#endif
#endif

    printf("checkpoint: joybus PIO\n");

    // Joybus RX (unchanged)
    uint offset_joybus = pio_add_program(pio_joybus, &joybus_program);
    joybus_rx_program_init(pio_joybus, sm_joybus, offset_joybus, PIN_JOYBUS_P1);
    pio_sm_set_enabled(pio_joybus, sm_joybus, true);
    joybus_rx_init(pio_joybus, sm_joybus);

    printf("checkpoint: audio DMA setup start\n");

#if AUDIO_ENABLED
#if !defined(N64_HDMI_AUDIO_SILENT)
    // ------------------------------------------------------------------
    // Audio DMA plumbing (unchanged from the original design):
    //
    //   1. dma_ch_audio_pio_data/ctrl: a self-chaining pair that keeps
    //      pulling words out of the audio PIO's RX FIFO into
    //      `last_audio_sample`, so there is always a location in RAM
    //      holding the *latest* captured N64 audio sample.
    //   2. dma_ch_audio_buffer_data/ctrl: a DMA-timer-paced self-chaining
    //      pair that copies `last_audio_sample` into the `audio_buffer`
    //      ring at the configured HDMI output sample rate.
    //
    // hdmi_audio_task() (registered above as pico_hdmi's background
    // task) then drains that ring into HDMI Data Islands.
    // ------------------------------------------------------------------
    uint dma_ch_audio_pio_data = dma_claim_unused_channel(true);
    uint dma_ch_audio_pio_ctrl = dma_claim_unused_channel(true);

    dma_channel_config c_audio_pio_data = dma_channel_get_default_config(dma_ch_audio_pio_data);
    channel_config_set_read_increment(&c_audio_pio_data, false);
    channel_config_set_dreq(&c_audio_pio_data, pio_get_dreq(pio_n64, sm_audio, false));
    channel_config_set_chain_to(&c_audio_pio_data, dma_ch_audio_pio_ctrl);
    channel_config_set_irq_quiet(&c_audio_pio_data, true);

    const volatile void *ptr_audio_pio_rxf = &pio_n64->rxf[sm_audio];
    dma_channel_configure(dma_ch_audio_pio_data, &c_audio_pio_data,
        &last_audio_sample, // Write to last_audio_sample
        ptr_audio_pio_rxf,  // Read from RX FIFO
        1,                  // Sample one full buffer
        false               // Do not start immediately
    );

    dma_channel_config c_audio_pio_ctrl = dma_channel_get_default_config(dma_ch_audio_pio_ctrl);
    channel_config_set_read_increment(&c_audio_pio_ctrl, false);
    channel_config_set_irq_quiet(&c_audio_pio_ctrl, true);
    dma_channel_configure(dma_ch_audio_pio_ctrl, &c_audio_pio_ctrl,
        &dma_hw->ch[dma_ch_audio_pio_data].al3_read_addr_trig,
        &ptr_audio_pio_rxf,
        1,
        true
    );

    uint dma_ch_audio_buffer_data = dma_claim_unused_channel(true);
    uint dma_ch_audio_buffer_ctrl = dma_claim_unused_channel(true);
    g_dma_ch_audio_buffer_data = dma_ch_audio_buffer_data;

    dma_channel_config c_audio_buffer_data = dma_channel_get_default_config(dma_ch_audio_buffer_data);
    channel_config_set_read_increment(&c_audio_buffer_data, false);
    channel_config_set_write_increment(&c_audio_buffer_data, true);
    channel_config_set_transfer_data_size(&c_audio_buffer_data, DMA_SIZE_32);
    channel_config_set_chain_to(&c_audio_buffer_data, dma_ch_audio_buffer_ctrl);
    channel_config_set_irq_quiet(&c_audio_buffer_data, true);
    // CRITICAL: pace this channel with DMA timer 0. Without a DREQ it stays
    // DREQ_FORCE and the whole ring transfers at max bus rate, re-triggered
    // endlessly by the ctrl channel - competing with the HSTX DMA channels
    // (0/1) for DMA controller service and starving the HSTX FIFO, which
    // shows up as "no signal" at 480p. This was the missing piece: the
    // timer fraction below is meaningless if the channel never waits on it.
    channel_config_set_dreq(&c_audio_buffer_data, dma_get_timer_dreq(0));

    dma_timer_claim(0);

    // dma_timer_set_fraction(0, numerator, denominator) paces the timer at
    // sys_clk * numerator / denominator (both 16-bit). These MUST be derived
    // from the ACTUAL system clock: 480P runs at 252 MHz (hstx_clk_div=2),
    // 240P at 126 MHz. The old fixed fractions assumed 126 MHz, so in 480P
    // the audio ring was being fed at exactly 2x the advertised rate. Reduce
    // the exact ratio sample_rate/sys_clk with Euclid's algorithm - every
    // supported rate reduces to a <=16-bit fraction at both 126 and 252 MHz.
    uint32_t sys_clk_hz = clock_get_hz(clk_sys);
    uint32_t a = g_config.audio_out_sample_rate;
    uint32_t b = sys_clk_hz;
    while (b) {
        uint32_t t = b;
        b = a % b;
        a = t;
    }
    dma_timer_set_fraction(0, g_config.audio_out_sample_rate / a, sys_clk_hz / a);

    volatile void *ptr_last_sample = &last_audio_sample;
    volatile void *ptr_audio_buffer = &audio_buffer[0];

    dma_channel_configure(dma_ch_audio_buffer_data, &c_audio_buffer_data,
        ptr_audio_buffer,  // Destination: ring buffer
        ptr_last_sample,   // Source: latest captured sample
        AUDIO_BUFFER_SIZE,
        false
    );

    dma_channel_config c_audio_buffer_ctrl = dma_channel_get_default_config(dma_ch_audio_buffer_ctrl);
    channel_config_set_read_increment(&c_audio_buffer_ctrl, false);
    channel_config_set_write_increment(&c_audio_buffer_ctrl, false);
    channel_config_set_irq_quiet(&c_audio_buffer_ctrl, true);
    dma_channel_configure(dma_ch_audio_buffer_ctrl, &c_audio_buffer_ctrl,
        &dma_hw->ch[dma_ch_audio_buffer_data].al2_write_addr_trig,
        &ptr_audio_buffer,
        1,
        true
    );

    // Start the ring buffer roughly half a buffer's worth ahead of the
    // capture DMA so hdmi_audio_task() never races the producer.
    audio_read_index = (AUDIO_BUFFER_SIZE / 2);
#endif // !N64_HDMI_AUDIO_SILENT
#endif // AUDIO_ENABLED

    printf("checkpoint: audio DMA setup done, entering capture loop\n");

#ifdef DIAGNOSTICS_JOYBUS
    uint32_t transfer = 0;
    uint32_t y = 0;

    gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "hello");
    gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "offset_joybus = %d", offset_joybus);

    while (true) {
        uint32_t value = joybus_rx_get_latest();

        y = 0;
        transfer++;

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: A=%d B=%d Z=%d Start=%d",
            transfer, !!A_BUTTON(value), !!B_BUTTON(value), !!Z_BUTTON(value), !!START_BUTTON(value));

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: DU=%d DD=%d DL=%d DR=%d",
            transfer, !!DU_BUTTON(value), !!DD_BUTTON(value), !!DL_BUTTON(value), !!DR_BUTTON(value));

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: Reset=%d", transfer, !!RESET_BUTTON(value));

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: TL=%d TR=%d",
            transfer, !!TL_BUTTON(value), !!TR_BUTTON(value));

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: CU=%d CD=%d CL=%d CR=%d",
            transfer, !!CU_BUTTON(value), !!CD_BUTTON(value), !!CL_BUTTON(value), !!CR_BUTTON(value));

        gfx_puttextf(0, y++ * 8, 0xffff, 0x0000, "%02d: X=%04d Y=%04d", transfer, X_STICK(value), Y_STICK(value));
    }
#endif

    // ------------------------------------------------------------------
    // Video capture loop (Core 0) - byte-for-byte the original N64 digital
    // RGB/sync decoder. This is completely independent of the HDMI output
    // backend: it just keeps g_framebuf filled with whatever the N64 is
    // currently drawing, and scanline_callback() (running on Core 1,
    // driven by pico_hdmi/HSTX) reads the freshest available line.
    // ------------------------------------------------------------------
#if defined(N64_HDMI_AUDIO_ONLY)
    // Audio-only test build: no video capture. Core 0 idles (the audio
    // capture DMA/timer runs autonomously; hdmi_audio_task on Core 1 feeds
    // the islands). The output shows the static test pattern.
    while (1) {
        tight_loop_contents();
    }
    __builtin_unreachable();
#else
    int count = 0;
    int row = 0;
    int column = 0;

#define CSYNCB_POS (0)
#define HSYNCB_POS (1)
#define CLAMPB_POS (2)
#define VSYNCB_POS (3)

#define CSYNCB_MASK (1 << CSYNCB_POS)
#define HSYNCB_MASK (1 << HSYNCB_POS)
#define CLAMPB_MASK (1 << CLAMPB_POS)
#define VSYNCB_MASK (1 << VSYNCB_POS)

#define ACTIVE_PIXEL_MASK (VSYNCB_MASK | HSYNCB_MASK | CLAMPB_MASK)

    uint32_t BGRS;
    uint32_t frame = 0;
    uint32_t crop_x = DEFAULT_CROP_X_PAL;
    uint32_t crop_y = DEFAULT_CROP_Y_PAL;

    while (1) {
        // Let the OSD code run
        osd_run();

        // Boot combo recovery (C-Up + C-Right at power-on = clean 480P).
        check_boot_combo();

        if (n64_reset_active) {
            uint32_t now = time_us_32();
            if (n64_reset_sync_back || (now - n64_reset_start_us > N64_RESET_MAX_US)) {
                // Fresh video again (or gave up): normal output resumes.
                n64_reset_active = false;
            }
        }

        // 1. Find posedge VSYNC
        if (n64_reset_active) {
            // The N64 is (supposedly) resetting. Poll instead of blocking so
            // the "Resetting..." overlay keeps animating, and flag when sync
            // actually disappears and when it comes back. Only RX samples are
            // consumed; once a fresh frame is captured the flag above clears.
            // First discard whatever the PIO buffered while the OSD menu was
            // open (capture is paused then, so the FIFO holds pre-reset video
            // that would otherwise be mistaken for a live frame).
            while (!pio_sm_is_rx_fifo_empty(pio_n64, sm_video)) {
                (void)pio_sm_get(pio_n64, sm_video);
            }
            uint32_t last_draw_us = 0;
            for (;;) {
                uint32_t t = time_us_32();
                if (!pio_sm_is_rx_fifo_empty(pio_n64, sm_video)) {
                    BGRS = pio_sm_get(pio_n64, sm_video);
                    if (BGRS & VSYNCB_MASK) {
                        if (n64_reset_sync_was_lost) {
                            n64_reset_sync_back = true; // video is back
                        } else if ((t - n64_reset_start_us) > N64_RESET_NO_DROP_US) {
                            // The console's video never actually stopped (some
                            // resets keep sync): consider it done anyway, so
                            // the reset state can't stick forever.
                            n64_reset_sync_back = true;
                        }
                        break; // begin capturing this frame
                    }
                }
                if ((t - last_sync_us) > N64_RESET_SYNC_LOST_US) {
                    n64_reset_sync_was_lost = true;
                    if ((int32_t)(t - last_draw_us) >= 0) {
                        draw_reset_overlay((t - n64_reset_start_us) / 1000u);
                        last_draw_us = t + 40 * 1000u;
                    }
                }
            }
        } else {
            do {
                BGRS = pio_sm_get_blocking(pio_n64, sm_video);
            } while (!(BGRS & VSYNCB_MASK));
        }
        last_sync_us = time_us_32();

        int active_row = 0;
        for (row = 0;; row++) {

            int skip_row = (
                (row % 2 != 0) ||            // Skip every second line (interlace)
                (row < crop_y) ||            // crop_y: rows to skip vertically from the top
                (active_row >= FRAME_HEIGHT) // Never write more rows than g_framebuf holds
            );

            // 2. Find posedge HSYNC
            if (n64_reset_active) {
                // Reset pending: poll so a mid-frame video drop bails out to
                // the overlay instead of blocking forever on the empty FIFO.
                for (;;) {
                    if (!pio_sm_is_rx_fifo_empty(pio_n64, sm_video)) {
                        BGRS = pio_sm_get(pio_n64, sm_video);

                        if ((BGRS & VSYNCB_MASK) == 0) {
                            goto end_of_line;
                        }
                        if ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK) break;
                    }
                    N64_RESET_BAIL_ON_SYNC_LOSS();
                }
            } else {
                do {
                    BGRS = pio_sm_get_blocking(pio_n64, sm_video);

                    if ((BGRS & VSYNCB_MASK) == 0) {
                        goto end_of_line;
                    }

                } while ((BGRS & ACTIVE_PIXEL_MASK) != ACTIVE_PIXEL_MASK);
            }

            if (skip_row) {
                if (n64_reset_active) {
                    for (;;) {
                        if (!pio_sm_is_rx_fifo_empty(pio_n64, sm_video)) {
                            BGRS = pio_sm_get(pio_n64, sm_video);

                            if ((BGRS & VSYNCB_MASK) == 0) {
                                goto end_of_line;
                            }
                            if ((BGRS & ACTIVE_PIXEL_MASK) != ACTIVE_PIXEL_MASK) break;
                        }
                        N64_RESET_BAIL_ON_SYNC_LOSS();
                    }
                } else {
                    do {
                        BGRS = pio_sm_get_blocking(pio_n64, sm_video);

                        if ((BGRS & VSYNCB_MASK) == 0) {
                            goto end_of_line;
                        }
                    } while ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK);
                }

                continue;
            }

            count = active_row * FRAME_WIDTH;
            int count_max = count + FRAME_WIDTH;
            active_row++;

            column = 0;

            // 3. Capture scanline

            // 3.1 Crop left black bar
            for (int left_ctr = 0; left_ctr < crop_x; left_ctr++) {
                BGRS = pio_sm_get_blocking(pio_n64, sm_video);
            };

            // 3.2 Capture active pixels
            BGRS = pio_sm_get_blocking(pio_n64, sm_video);

            // This code is duplicated for performance reasons - 555 and 565
            // respectively. Both variants now capture the "discarded" second
            // sample per column too (the n64_diag_noskip change): the N64
            // digital tap sends 2 unique samples per displayed pixel column,
            // and the old code threw the second one away, throwing away half
            // the real horizontal resolution. The PIO-side timing budget is
            // unchanged - same number of pio_sm_get_blocking() calls per
            // column as before, and n64_diag_noskip proved this exact loop
            // shape keeps up with the N64's pixel clock on this hardware.

            if (g_config.dvi_color_mode == DVI_RGB_555) {
                do {
                    // 3.3 Convert to RGB555 (stored packed in a uint16, top bit unused)
                    g_framebuf[count] = (
                        ((BGRS <<  1) & 0xf800) |
                        ((BGRS >> 12) & 0x07e0) |
                        ((BGRS >> 26) & 0x001f)
                    );
                    count++;

                    if (count >= count_max) {
                        do {
                            BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                            N64_RESET_BAIL_ON_SYNC_LOSS();
                        } while ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK);
                        break;
                    }

                    // 3.4 Second sample - previously discarded, now captured
                    BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                    g_framebuf[count] = (
                        ((BGRS <<  1) & 0xf800) |
                        ((BGRS >> 12) & 0x07e0) |
                        ((BGRS >> 26) & 0x001f)
                    );
                    count++;

                    if (count >= count_max) {
                        do {
                            BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                            N64_RESET_BAIL_ON_SYNC_LOSS();
                        } while ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK);
                        break;
                    }

                    // 3.5 Count number of pixels processed on this row
                    column += 2;

                    BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                } while (1);
            } else if (g_config.dvi_color_mode == DVI_RGB_565) {
                do {
                    // 3.3 Convert to RGB565
                    g_framebuf[count] = (
                        ((BGRS <<  1) & 0xf800) |
                        ((BGRS >> 12) & 0x07c0) |
                        ((BGRS >> 26) & 0x001f)
                    );
                    count++;

                    if (count >= count_max) {
                        do {
                            BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                            N64_RESET_BAIL_ON_SYNC_LOSS();
                        } while ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK);
                        break;
                    }

                    // 3.4 Second sample - previously discarded, now captured
                    BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                    g_framebuf[count] = (
                        ((BGRS <<  1) & 0xf800) |
                        ((BGRS >> 12) & 0x07c0) |
                        ((BGRS >> 26) & 0x001f)
                    );
                    count++;

                    if (count >= count_max) {
                        do {
                            BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                            N64_RESET_BAIL_ON_SYNC_LOSS();
                        } while ((BGRS & ACTIVE_PIXEL_MASK) == ACTIVE_PIXEL_MASK);
                        break;
                    }

                    column += 2;
                    BGRS = pio_sm_get_blocking(pio_n64, sm_video);
                } while (1);
            } else {
                // Unsupported mode - keep the last frame rather than corrupt it.
            }
        }

end_of_line:
        // Crop region: forced by the OSD's "Video Region" item when set, else
        // auto-detect NTSC / PAL based on number of rows.
        if (g_config.video_region == VIDEO_REGION_NTSC) {
            crop_x = DEFAULT_CROP_X_NTSC;
            crop_y = DEFAULT_CROP_Y_NTSC;
        } else if (g_config.video_region == VIDEO_REGION_PAL) {
            crop_x = DEFAULT_CROP_X_PAL;
            crop_y = DEFAULT_CROP_Y_PAL;
        } else if (IN_TOLERANCE(row, ROWS_PAL, ROWS_TOLERANCE)) {
            crop_x = DEFAULT_CROP_X_PAL;
            crop_y = DEFAULT_CROP_Y_PAL;
        } else {
            // In case the mode can't be detected, default to NTSC as it crops fewer rows
            crop_x = DEFAULT_CROP_X_NTSC;
            crop_y = DEFAULT_CROP_Y_NTSC;
        }

        frame++;
    }

    __builtin_unreachable();
#endif // N64_HDMI_AUDIO_ONLY
}
