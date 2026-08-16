#pragma once

#include "pico_hdmi/video_output_rt.h"

// 480p mode descriptor that this project confirmed working on this
// hardware (800x525, div=2 -> 25.2 MHz at a 252 MHz sys_clk). Always
// defined, so the 3-mode overlay app can boot the saved output_mode from
// config. In this SINGLE-mode app (N64_HDMI_SINGLE_MODE) the descriptor is
// only reachable when the build IS 480P (main.c's compile-time boot_mode_sel);
// the other builds leave it compiled-but-unreachable.
extern const video_mode_t n64_hdmi_480p_confirmed;

// Persist g_config (incl. output_mode) to flash and reboot the Pico.
// The boot-time init then configures the HDMI output for the saved mode
// from scratch, which is far more reliable on real monitors than the
// mid-stream video_output_set_mode() live switch (that one drops sync on
// some sinks and never re-acquires). In this single-mode app the persisted
// output_mode is forced back to the compile-time mode on load, so a reboot
// always comes back in the same mode.
void app_save_config_and_reboot(void);

// Pulse GPIO 21 high to reset the N64 from the OSD menu.
void app_reset_n64(void);
