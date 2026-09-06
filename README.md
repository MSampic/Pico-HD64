# n64-picohdmi

N64 digital video/audio capture on a Raspberry Pi Pico 2 (RP2350), output as
HDMI through the HSTX peripheral.

The capture logic comes from
[PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64) (the original
Pico/RP2040 project, of which this repository is a fork); the output stage
replaces its PIO-bit-banged TMDS with
[`pico_hdmi`](https://github.com/fliperama86/pico_hdmi)'s HSTX hardware TMDS
encoder. No overclock is needed for 240p output.

## How it works

- **Capture (Core 0).** PIO1 samples the N64's digital video/audio encoder
  pins line by line into a RAM framebuffer. The N64's serial audio DAC lines
  are captured as I2S (LRCLK/SDAT/BCLK).
- **Output (Core 1).** `pico_hdmi` drives the HSTX peripheral, encodes TMDS
  in hardware, and emits HDMI (DVI-style RGB) with audio carried as HDMI Data
  Island packets.
- **OSD.** The on-screen menu is painted by the scanline callback directly
  over the live capture (a box-blit, not a frozen frame). Settings live in a
  small config saved to flash.
- **Controller.** Joybus P1 is serviced by the firmware, so the OSD is
  navigated with the controller — no extra hardware.
- No genlock between the N64 clock and the HDMI clock; the output always
  reads the freshest captured line (occasional tearing is inherent).

## Two firmware variants

Exactly two apps, each with its own **standalone** build (they need
different `pico_hdmi` library options, so there is no top-level app build):

| App | Output modes | OSD |
|---|---|---|
| [`software/apps/n64_hdmi`](software/apps/n64_hdmi) | **One**, fixed at compile time (`N64_HDMI_MODE`): 240P / 480P / 720P | Picture settings only, no mode switch |
| [`software/apps/n64_hdmi_osd_overlay`](software/apps/n64_hdmi_osd_overlay) | **Three**, selectable at runtime from the OSD (240P / 480P / 720P), with boot-combo recovery | Picture settings + "Video Mode" + "Save & Reboot" |

Both share the same source files; the single-mode define
(`N64_HDMI_SINGLE_MODE`) compiles out the multi-mode-only parts.

## N64_hdmi_osd_overlay variant

This app has boot combo reset using joybus in case of failure or incompatible video mode:

| Chord | Target mode |
|---|---|
| C-Up + C-Right | 720P |
| C-Up + C-Down | 480P |
| C-Up + C-Left | 240P |

## Known bugs

- OSD text unreadable on 240p mode
- 720p is not crisp horizontally as 240p or 480p
- OSD menu opens even when not pressing the button combination

## Repository layout

```
software/
├── apps/            # firmware apps (n64_hdmi, n64_hdmi_osd_overlay)
├── assets/          # shared assets (font_8x8.h)
└── lib/             # vendored libraries (pico_hdmi)
hardware/            # PicoDVI-N64 board KiCad files
img/                 # images
```

## Wiring

GPIO map (same N64-side wiring as the original PicoDVI-N64 board — see its
`hardware/` KiCad files and `img/wiring.jpg` for the exact tap points into
the N64's video encoder):

| Pins | Function | Signal |
|---|---|---|
| GPIO 0–6 | Video data | N64 `D0`–`D6` (digital video encoder) |
| GPIO 7 | Video sync | N64 `DSYNC` |
| GPIO 8 | Video clock | N64 `CLK` |
| GPIO 9 | Audio LRCLK | N64 serial audio DAC |
| GPIO 10 | Audio data | N64 serial audio DAC `SDAT` |
| GPIO 11 | Audio bit clock | N64 serial audio DAC `BCLK` |
| GPIO 20 | Controller 1 | N64 Joybus |
| GPIO 21 | N64 Reset | N64 PIF |
| GPIO 12–19 | HDMI output | HSTX TMDS pairs (see below) |
| GPIO 28 | UART TX | Debug log, 115200 baud |

**HDMI output pins (GPIO 12–19)** use `dvisock`'s default HSTX pinout,
which matches this board's actual wiring — `main.c` does **not** call
`video_output_set_hstx_pinout()`:

```
GPIO12 = D0+    GPIO13 = D0-
GPIO14 = CLK+     GPIO15 = CLK-
GPIO16 = D2+     GPIO17 = D2-
GPIO18 = D1+     GPIO19 = D1-
```

This is the old PicoDVI-N64 "pico_sock_cfg" order
(CLK=14, D0=12, D1=18, D2=16). If you build your own board with a different HSTX wiring
you must call `video_output_set_hstx_pinout()` accordingly.

## Building

Requires `arm-none-eabi-gcc`, `cmake`, `ninja`, and the
[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (RP2350
support), plus `PICO_SDK_PATH` set.

### Direct CMake

Each app builds standalone:

```bash
cmake -S software/apps/n64_hdmi -B build-n64-hdmi-720p -G Ninja \
      -DPICO_SDK_PATH=$PICO_SDK_PATH -DN64_HDMI_MODE=720P
ninja -C build-n64-hdmi-720p
```

### CMake presets

The **output mode is configurable from CMake** via presets, one file per app
(the Pico SDK must be reachable at `PICO_SDK_PATH`):

```bash
cd software/apps/n64_hdmi
cmake --preset 240p && cmake --build --preset 240p     # or 480p / 720p
```

```bash
cd software/apps/n64_hdmi_osd_overlay
cmake --preset 240p-default && cmake --build --preset 240p-default
```

Each app's README documents its full set of flags (`N64_HDMI_MODE`,
`N64_HDMI_240P_OC252`, audio isolation options, `N64_HDMI_OSD_FORCE_OPEN`,
defaults, etc.):

- [software/apps/n64_hdmi/README.md](software/apps/n64_hdmi/README.md)
- [software/apps/n64_hdmi_osd_overlay/README.md](software/apps/n64_hdmi_osd_overlay/README.md)

## Flashing

Hold BOOTSEL on the Pico 2 and drag the `.uf2` onto the mass-storage device
that appears. All modes execute from RAM (`copy_to_ram`) because the 480p/720p
modes overclock the flash XIP beyond its safe read timing.

## License

- N64 capture code: BSD-3-Clause, Copyright (c) 2023 Konrad Beckmann (from
  [PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64)).
- `pico_hdmi`: Unlicense (from
  [fliperama86/pico_hdmi](https://github.com/fliperama86/pico_hdmi)).
- Port glue in `main.c`/`config.h`/CMake files: same BSD-3-Clause terms as
  the code they extend.
