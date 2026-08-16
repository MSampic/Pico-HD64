# apps/n64_hdmi_osd_overlay

N64 capture + HDMI output on a Raspberry Pi Pico 2 (RP2350), with **three**
output modes (240P / 480P / 720P) selectable **at runtime** from the OSD menu
("Video Mode" + "Save & Reboot"), plus boot-combo recovery.

The OSD paints over the **live** capture (the scanline callback box-blits the
pre-rendered menu over each line), so the game keeps running visibly behind
the menu. Picture settings are saved to flash, and N64 audio is carried over
HDMI as Data Island packets.

The sources are shared with [`apps/n64_hdmi`](../n64_hdmi) (the single-mode
variant, `N64_HDMI_SINGLE_MODE`), which compiles out the multi-mode parts.

## Build

Standalone app (own `pico_sdk_init()` + vendored `lib/pico_hdmi`).

Prerequisites: `arm-none-eabi-gcc`, `cmake`, `ninja`, the
[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (RP2350),
and `PICO_SDK_PATH` set.

### CMake presets (recommended)

The boot default mode is chosen by preset, from inside this directory (all
three modes are still selectable at runtime in any build):

```bash
cd software/apps/n64_hdmi_osd_overlay
cmake --preset 240p-default      # or 480p-default / 720p-default
cmake --build --preset 240p-default
```

Output: `build/<preset>/n64_hdmi_osd_overlay.uf2`

### CMake

```bash
cmake -S software/apps/n64_hdmi_osd_overlay -B build-osd-3modes -G Ninja \
      -DPICO_SDK_PATH=$PICO_SDK_PATH -DN64_HDMI_MODE=240P
ninja -C build-osd-3modes
```

## Output modes

All three modes are available at runtime regardless of build; `N64_HDMI_MODE`
only picks the **boot default**.

| Mode | Output | Notes |
|---|---|---|
| 240P (boot default) | true 240p, 1:1 with the captured field | stock 126 MHz sys_clk; with `N64_HDMI_240P_OC252` (on by default) runs 252 MHz but keeps the identical 25.2 MHz wire signal |
| 480P | 640x480, 2x upscale | 252 MHz sys_clk, VREG 1.15V, runs from RAM |
| 720P | 1280x720, 2x/3x upscale | 372 MHz sys_clk, VREG 1.30V, runs from RAM |

480P/720P require `copy_to_ram` (already forced by this CMakeLists): at the
raised clock the flash XIP read timing no longer samples correctly.

A mode change is **staged** in the OSD and only applied by **"Save & Reboot"**
(persist to flash + reboot): switching modes live drops sync on some monitors
and never recovers.

### Boot-combo recovery

Holding **C-Up + one C button** at power-on resets the config to clean
defaults for that mode and reboots — a safe way out if a bad saved setting
leaves the display black (e.g. a mode a monitor can't show):

| Chord | Result |
|---|---|
| C-Up + C-Left | Clean 240P defaults |
| C-Up + C-Down | Clean 480P defaults |
| C-Up + C-Right | Clean 720P defaults |

(If the current saved mode already matches the chord, nothing happens — no
reboot.)

## Build flags

All flags are CMake cache variables (`-D<FLAG>=...`); `ON`/`OFF` values are
booleans.

| Flag | Default | Description |
|---|---|---|
| `N64_HDMI_MODE` | `240P` | **Boot default** output mode: `240P`, `480P` or `720P`. All three remain switchable at runtime. |
| `N64_HDMI_720P_BUILD` | `ON` | Build with 720p support: enables runtime mode attributes (positive sync polarity / back-porch Data Island placement from the mode descriptor) and the active-line double buffer that gives the scanline callback a pipelined budget at 720P. `OFF` compiles the library exactly like the pre-720p builds — isolation control to bisect the 480P rolling-band regression. |
| `N64_HDMI_240P_OC252` | `ON` | 240p mode: run sys_clk at 252 MHz. Keeps the 25.2 MHz 240p wire signal (div=2) while giving the N64 capture loop enough CPU headroom; stock 126 MHz 240p shows garbage-with-the-game's-colors on real hardware. |
| `N64_HDMI_OSD_FORCE_OPEN` | `OFF` | Diagnostic: open the OSD overlay once at boot (and only then) so the menu can be seen without a controller. |
| `CONFIG_DEFAULT_SAMPLE_RATE_HZ` | `SAMPLE_RATE_48000_HZ` | Default HDMI audio sample rate: `SAMPLE_RATE_32000_HZ`, `SAMPLE_RATE_44100_HZ`, `SAMPLE_RATE_48000_HZ` or `SAMPLE_RATE_96000_HZ`. |
| `CONFIG_DEFAULT_COLOR_DEPTH` | `DVI_RGB_565` | Default color depth: `DVI_RGB_565` or `DVI_RGB_555`. |
| `N64_HDMI_AUDIO_ENABLED` | `ON` | Build with HDMI audio (N64 audio -> Data Islands). `OFF` drops all audio DMA/background-task machinery — useful to isolate whether audio (running continuously on Core 1, competing with HSTX timing) causes picture instability. |
| `N64_HDMI_AUDIO_SILENT` | `OFF` | Audio machinery ON but pushing silence packets: keeps the DI queue/ACR/infoframes identical to the real-audio build, but no N64 capture hardware runs (no audio PIO SM, no DMA/timer ring). Isolation test for "480p + audio = no signal". Requires `N64_HDMI_AUDIO_ENABLED=ON`. |
| `N64_HDMI_AUDIO_SILENT_SM1` | `OFF` | Silent build that additionally keeps audio PIO SM1 enabled on PIO1 (no DMA) — isolates whether SM1's mere presence (PIO1 round-robin shared with video capture) breaks the picture. Requires `N64_HDMI_AUDIO_SILENT=ON`. |
| `N64_HDMI_AUDIO_ONLY` | `OFF` | Audio-only test: no N64 video capture at all, scanline callback draws a static bar pattern, full real audio capture runs. Answers "does N64 audio -> HDMI islands produce sound at all?". Requires `N64_HDMI_AUDIO_ENABLED=ON`; mutually exclusive with `N64_HDMI_AUDIO_SILENT`. |
| `PICO_SDK_PATH` | — | Path to the Pico SDK (also read from the `PICO_SDK_PATH` env var). |

## Flashing

Hold BOOTSEL on the Pico 2 and drag `build/<preset>/n64_hdmi_osd_overlay.uf2`
onto the mass-storage device that appears.

## OSD

Open the menu with **L + R + C-Down + D-Pad Down**.

| Buttons | Action |
|---|---|
| D-Pad Up/Down | Move focus |
| D-Pad Left/Right | Change the focused value |
| A | Enter menu / activate item |
| (B or the shortcut) | Close |

Menu items: **Video Mode** (240P/480P/720P — staged, applied on reboot),
**Zoom** (50–400%), **Pos X** (–300..300, per-pixel),
**Pos Y** (–110..110, per-pixel), **Blur** (ON/OFF),
**Aspect** (16:9 / 4:3, only visible at 720P),
**Video Region** (Auto / NTSC / PAL — forces the capture crop, applies
live; Auto keeps the frame-rate detection), **Reset N64**,
**Save & Reboot**, **Exit OSD**.

## Wiring

See the root [README](../../README.md) — this app uses the same GPIO map
(0–11 N64 capture, 20 Joybus P1, 12–19 HSTX HDMI output with pico_hdmi's
default pinout).
