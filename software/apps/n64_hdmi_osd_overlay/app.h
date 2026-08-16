#pragma once

#include "pico_hdmi/video_output_rt.h"

// 480p mode descriptor that this project confirmed working on this
// hardware (800x525, div=2 -> 25.2 MHz at a 252 MHz sys_clk). Always
// defined, so any build can boot the saved output_mode from config.
extern const video_mode_t n64_hdmi_480p_confirmed;

// Persist g_config (incl. output_mode) to flash and reboot the Pico.
// The boot-time init then configures the HDMI output for the saved mode
// from scratch, which is far more reliable on real monitors than the
// mid-stream video_output_set_mode() live switch (that one drops sync on
// some sinks and never re-acquires).
void app_save_config_and_reboot(void);

// Pulse GPIO 21 high to reset the N64 from the OSD menu.
void app_reset_n64(void);
