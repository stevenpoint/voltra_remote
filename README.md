# Voltra Remote

A dedicated Bluetooth remote for the **Beyond Power Voltra I**, built on the
[Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8):
a round touchscreen with a rotary knob and haptics. Set the weight, load and unload,
and control chains, inverse chains, mountain, and eccentric, without reaching for the
Voltra's screen or your phone.

> **Unofficial.** Voltra Remote is an independent project, not affiliated with or endorsed
> by Beyond Power. It drives the Voltra through a reverse-engineered protocol, and a
> firmware update could change that protocol. Use it at your own risk, and keep clear of
> the cable when loading.

```
            ┌──────────────────────────┐
            │       ᛒ   ▮▮▮ 78%        │   <- Voltra battery; tap: connect menu
            │      Tap to Unload       │
            │                          │
            │          45  lb          │   <- turn: weight · tap: load / unload
            │ (60)                (90) │      hold: auto load
            │                          │   <- presets: tap to send, hold to store
            │  ┌────┐  ┌────┐  ┌────┐  │
            │  │ ⩔  │  │ ⁘  │  │ 8  │  │   <- active accessories either side of
            │  │+25%│  └────┘  │10% │  │      the settings button
            │  └────┘          └────┘  │
            └──────────────────────────┘
```

## Features

- **Weight on the dial.** Turn slowly for 1 lb steps, spin for 5 lb steps that snap to
  multiples of 5. The limit is read from the Voltra (230 lb on current firmware).
- **Load, unload and auto load.** Tap the weight to load or unload. Press and hold it to
  start the Voltra's own auto load: pull the cable out and hold it, and it loads after a
  3-second countdown that the knob shows too.
- **Always in step with the Voltra.** The screen follows what the Voltra reports, so
  changes made on the Voltra itself show up on the knob. The weight is a dim green while
  unloaded and bright green once loaded, and resting between sets still counts as loaded.
- **Set screen.** During a set, everything else clears away and the set and rep counts
  fill the lower half. Until the first rep, the rep count pulses like the Voltra's.
- **Two weight presets.** Hold a preset to store the current weight, tap it to send that
  weight. They are stored on the knob and survive a restart.
- **Accessories** with the Voltra's own icons: eccentric, chains, inverse chains and
  mountain. The last three are mutually exclusive, as on the Voltra. Amounts follow the
  Voltra's lb/% setting, with the other unit shown alongside.
- **Status at a glance.** Bluetooth icon (blue when connected), the Voltra's battery, and
  the knob's own battery. Haptic clicks confirm every action.

## What you need

- A Beyond Power **Voltra I**. Tested against current firmware, with the 230 lb limit.
- A **Waveshare ESP32-S3-Knob-Touch-LCD-1.8**.
- A USB-C data cable, and a computer with [PlatformIO](https://platformio.org/) (the
  VS Code extension, or the `pio` command line).

## Installing

1. Clone this repository and open a terminal in it.
2. Connect the knob over USB-C. The board has two chips behind a USB switch: the ESP32-S3
   shows up as an Espressif USB device (VID:PID `303A:1001`). If you get a CH340 serial
   port instead, flip the USB-C plug over.
3. Build and flash:

   ```bash
   pio run -e remote -t upload
   ```

   The first build downloads the toolchain and libraries, which takes a few minutes.

**Windows:** build from PowerShell or cmd, not Git Bash/MSYS. The toolchain installer
refuses to run under MSYS and leaves the compiler missing, so the build then fails
with `'xtensa-esp32s3-elf-gcc' is not recognized`.

Each build also writes a single flashable image, `.pio/build/remote/firmware.factory.bin`,
which can be written at offset `0x0` with any ESP32 flasher, such as
[ESP Web Flasher](https://espressif.github.io/esptool-js/).

## Using it

**First connection.** Switch the Voltra on and tap the top of the knob's screen (or the
weight, while disconnected) to open the connect menu. Tap your Voltra (`VTR-…`) in the
list. The knob remembers it and reconnects by itself from then on. To disconnect, open
the connect menu again.

**Main screen**

| Do this | To |
|---|---|
| Turn the knob | set the weight (slow: 1 lb, fast: 5 lb) |
| Tap the weight | load / unload |
| Press and hold the weight | start auto load; tap again to cancel it before it loads |
| Tap a preset | send its weight |
| Press and hold a preset | store the current weight in it |
| Tap an accessory bubble | adjust that accessory |
| Tap the five-dot button | open the accessory settings |

**Accessory settings.** Four bubbles: Eccentric, Chains, Mountain and Inverse. Tap one
and turn the knob to set its amount, then tap Done. Dialling an amount into Chains,
Mountain or Inverse switches the Voltra to that style and turns the other two off. The
five-dot button turns green while any accessory is on.

## Troubleshooting

- **The Voltra doesn't appear in the connect menu.** Make sure it is switched on, and
  close the Beyond+ app on your phone if it is connected: the Voltra may not advertise
  while another app holds the connection.
- **Turning the knob does nothing, or double-counts.** Build with `-DKNOB_QUADRATURE=1`
  in `platformio.ini`. If it counts backwards, add `-DKNOB_INVERT=1`.
- **Something on the knob disagrees with the Voltra.** The knob re-reads the Voltra
  every few seconds; if it stays wrong, open an issue with what you did on each device.

## Development

| Path | What |
|---|---|
| `src/voltra/voltra_protocol.*` | Frame builder/parser, CRCs, parameter registry. No Arduino dependencies, so it is unit-tested on the host. |
| `src/voltra/voltra_client.*` | NimBLE central: scan, connect, handshake, paced writes, load handling, device state. |
| `src/ui/ui.cpp` | LVGL 8 screens: main, set, accessory settings, adjust dial, connect menu. |
| `src/ui/knob_step.h` | Fine/coarse knob stepping and snapping (unit-tested). |
| `src/ui/fonts/` | Generated fonts: Poppins text and numbers, and the icon font. |
| `src/hw/` | Board drivers: QSPI ST77916 panel and LVGL port, CST816 touch, knob, DRV2605 haptics, battery. |
| `include/lv_conf.h` | LVGL configuration. |
| `tools/` | Font and icon generators, the diagnostic sweep generator, and the Poppins font files. |
| `test/test_protocol/` | Unity tests, mostly against frames captured from the Voltra and the official app. |
| `docs/PROTOCOL.md` | Everything known about the Voltra's BLE protocol. |

**Tests.** 25 host-side tests cover the frame builder and parser against captured
frames, the knob stepping, and the load, auto-load and workout-status logic:

```bash
pio test -e native
```

**Builds.** `remote` is the release build and prints nothing over serial. `remote_diag`
adds full logging and a register sweep for protocol work; see
[docs/PROTOCOL.md](docs/PROTOCOL.md#finding-undocumented-registers).

**Fonts and icons.** The generated files are committed; regenerate them only to change a
size or a shape. Both generators need Python with Pillow. The UI is set in Poppins:
Medium for text, Bold for numbers. The text sizes fall back to LVGL's built-in
Montserrat for the Bluetooth and battery symbols, which Poppins lacks.

```bash
# text, 14/16/18/20/22 px (16 shown)
python tools/gen_font.py --ttf tools/Poppins-Medium.ttf --size 16 --ascii \
    --fallback lv_font_montserrat_16 --name font_poppins_16 --out src/ui/fonts/font_poppins_16.c
# the weight and dial values, and the set / rep counters
python tools/gen_font.py --ttf tools/Poppins-Bold.ttf --size 96 --chars "0123456789-+" \
    --name font_poppins_96 --out src/ui/fonts/font_poppins_96.c
python tools/gen_font.py --ttf tools/Poppins-Bold.ttf --size 64 --chars "0123456789" \
    --name font_poppins_bold_64 --out src/ui/fonts/font_poppins_bold_64.c
# icons: drawn from shapes, at U+E000-U+E004 (the ICON_* macros in ui.cpp)
python tools/gen_icons.py --size 26 --name font_icons_26 --out src/ui/fonts/font_icons_26.c --preview icons.png
python tools/gen_icons.py --size 18 --name font_icons_18 --out src/ui/fonts/font_icons_18.c
```

**Board pins** (ESP32-S3 side):

| Function | GPIO |
|---|---|
| LCD QSPI CS / SCK / D0–D3 / RST / backlight | 14 / 13 / 15, 16, 17, 18 / 21 / 47 |
| I²C SDA / SCL (CST816 touch `0x15`, DRV2605 haptics `0x5A`) | 11 / 12 |
| Touch INT / RST | 9 / 10 |
| Knob A / B | 8 / 7 |
| Battery ADC (10k/10k divider) | 1 |

## License

Voltra Remote is released under the [MIT License](LICENSE). The generated font files in
`src/ui/fonts/font_poppins_*.c` embed glyphs from Poppins and remain under the SIL Open
Font License; the third-party components below keep their own licences.

## Credits

The protocol work builds on community reverse-engineering by
[dylanmaniatakes/Beyond-Power-HomeAssistant](https://github.com/dylanmaniatakes/Beyond-Power-HomeAssistant),
[dylanmaniatakes/Beyond-Power-Voltra-Android](https://github.com/dylanmaniatakes/Beyond-Power-Voltra-Android)
and [jpamorgan/voltra-sdk](https://github.com/jpamorgan/voltra-sdk). The accessory and
settings icons are redrawn after the Voltra's own.

Third-party components:

- [LVGL](https://lvgl.io/): MIT License.
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino): Apache License 2.0.
- [Poppins](https://github.com/itfoundry/Poppins) (via Google Fonts) and LVGL's built-in
  Montserrat: SIL Open Font License 1.1 (`tools/Poppins-OFL.txt`).
- [Arduino core for ESP32](https://github.com/espressif/arduino-esp32): LGPL 2.1; ESP-IDF: Apache License 2.0.

Beyond Power and Voltra are trademarks of their respective owner, used here only to
identify the device this remote works with.
