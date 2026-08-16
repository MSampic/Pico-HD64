# apps/n64_hdmi

N64 capture + HDMI output on a Raspberry Pi Pico 2 (RP2350), with exactly
**one** output mode fixed at compile time. This is the "single mode" variant
of the shared firmware sources; the other variant is
[`apps/n64_hdmi_osd_overlay`](../n64_hdmi_osd_overlay), which keeps all
three modes selectable at runtime.
Because `N64_HDMI_SINGLE_MODE` is defined here, this app compiles out the
multi-mode-only parts:

- no **"Video Mode"** item in the OSD menu;
- no **boot-combo recovery** (C-Up + C-Left/Down/Right at power-on);
- `config_load()` forces the saved `output_mode` back to the compile-time
  mode on every boot, so only picture settings (zoom, position, blur,
  aspect) carry over — even from a config saved by the overlay app.

Everything else is the same as the overlay app: the OSD paints over the live
capture (box-blit in the scanline callback), picture settings are saved to
flash, and N64 audio is carried over HDMI as Data Island packets.

## Build

Standalone app (own `pico_sdk_init()` + vendored `lib/pico_hdmi`).

Prerequisites: `arm-none-eabi-gcc`, `cmake`, `ninja`, the
[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (RP2350),
and `PICO_SDK_PATH` set.

### CMake presets (recommended)

The output mode is chosen by preset, from inside this directory:

```bash
cd software/apps/n64_hdmi
cmake --preset 240p        # or 480p / 720p
cmake --build --preset 240p
```

Output: `build/<preset>/n64_hdmi.uf2`

### CMake

```bash
cmake -S software/apps/n64_hdmi -B build-n64-hdmi-720p -G Ninja \
      -DPICO_SDK_PATH=$PICO_SDK_PATH -DN64_HDMI_MODE=720P
ninja -C build-n64-hdmi-720p
```## Output modes

Pick exactly one with `N64_HDMI_MODE` (also selectable via presets):

| Value | Output | Notes |
|---|---|---|
| `240P` (default) | true 240p, 1:1 with the captured field | stock 126 MHz sys_clk; with `N64_HDMI_240P_OC252` (on by default) runs 252 MHz but keeps the identical 25.2 MHz wire signal |
| `480P` | 640x480, 2x upscale | 252 MHz sys_clk, VREG 1.15V, runs from RAM |
| `720P` | 1280x720, 2x/3x upscale | 372 MHz sys_clk, VREG 1.30V, runs from RAM |

480P/720P require `copy_to_ram` (already forced by this CMakeLists): at the
raised clock the flash XIP read timing no longer samples correctly.

## Build flags

All flags are CMake cache variables (`-D<FLAG>=...`); `ON`/`OFF` values are
booleans.

| Flag | Default | Description |
|---|---|---|
| `N64_HDMI_MODE` | `240P` | Single compiled output mode: `240P`, `480P` or `720P` (fixed at compile time, not switchable at runtime). |
| `N64_HDMI_240P_OC252` | `ON` | 240p mode: run sys_clk at 252 MHz. Keeps the 25.2 MHz 240p wire signal (div=2) while giving the N64 capture loop enough CPU headroom; stock 126 MHz 240p shows garbage-with-the-game's-colors on real hardware. Ignored in 480P/720P builds. |
| `CONFIG_DEFAULT_SAMPLE_RATE_HZ` | `SAMPLE_RATE_48000_HZ` | Default HDMI audio sample rate: `SAMPLE_RATE_32000_HZ`, `SAMPLE_RATE_44100_HZ`, `SAMPLE_RATE_48000_HZ` or `SAMPLE_RATE_96000_HZ`. |
| `CONFIG_DEFAULT_COLOR_DEPTH` | `DVI_RGB_565` | Default color depth: `DVI_RGB_565` or `DVI_RGB_555`. |
| `N64_HDMI_AUDIO_ENABLED` | `ON` | Build with HDMI audio (N64 audio -> Data Islands). `OFF` drops all audio DMA/background-task machinery — useful to isolate whether audio (running continuously on Core 1, competing with HSTX timing) causes picture instability. |
| `N64_HDMI_AUDIO_SILENT` | `OFF` | Audio machinery ON but pushing silence packets: keeps the DI queue/ACR/infoframes identical to the real-audio build, but no N64 capture hardware runs (no audio PIO SM, no DMA/timer ring). Isolation test for "480p + audio = no signal". Requires `N64_HDMI_AUDIO_ENABLED=ON`. |
| `N64_HDMI_AUDIO_SILENT_SM1` | `OFF` | Silent build that additionally keeps audio PIO SM1 enabled on PIO1 (no DMA) — isolates whether SM1's mere presence (PIO1 round-robin shared with video capture) breaks the picture. Requires `N64_HDMI_AUDIO_SILENT=ON`. |
| `N64_HDMI_AUDIO_ONLY` | `OFF` | Audio-only test: no N64 video capture at all, scanline callback draws a static bar pattern, full real audio capture runs. Answers "does N64 audio -> HDMI islands produce sound at all?". Requires `N64_HDMI_AUDIO_ENABLED=ON`; mutually exclusive with `N64_HDMI_AUDIO_SILENT`. |
| `PICO_SDK_PATH` | — | Path to the Pico SDK (also read from the `PICO_SDK_PATH` env var). |

## Flashing

Hold BOOTSEL on the Pico 2 and drag `build/<preset>/n64_hdmi.uf2` onto the
mass-storage device that appears.

## OSD

Open the menu with **L + R + C-Down + D-Pad Down**.

| Buttons | Action |
|---|---|
| D-Pad Up/Down | Move focus |
| D-Pad Left/Right | Change the focused value |
| A | Enter menu / activate item |
| (B or the shortcut) | Close |

Menu items: **Zoom** (50–400%), **Pos X** (–300..300, per-pixel),
**Pos Y** (–110..110, per-pixel), **Blur** (ON/OFF), **Aspect**
(16:9 / 4:3, only visible at 720P), **Video Region** (Auto / NTSC / PAL —
forces the capture crop, applies live; Auto keeps the frame-rate
detection), **Reset N64**, **Save & Reboot**, **Exit OSD**. There is no
"Video Mode" item in this variant. `N64_HDMI_SINGLE_MODE` also removes the
boot-combo recovery the overlay app has.

## Wiring

See the root [README](../../README.md) — this app uses the same GPIO map
(0–11 N64 capture, 20 Joybus P1, 12–19 HSTX HDMI output with pico_hdmi's
default pinout).
