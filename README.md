# Voltra Watch

Bar remote for a Beyond Power VOLTRA I on the Waveshare **ESP32-S3-Touch-AMOLED-2.06** watch.

Not affiliated with Beyond Power or Waveshare.

A port of [kirby6365/voltra_remote](https://github.com/kirby6365/voltra_remote) (MIT), the
Voltra remote for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8, to the 2.06" watch. The knob
build is kept alongside. Protocol work builds on Omar Shahine's
[voltra-knob](https://github.com/omarshahine/voltra-knob), dylanmaniatakes'
Beyond-Power-HomeAssistant / Beyond-Power-Voltra-Android and jpamorgan's voltra-sdk.

## What it does

| Gesture | Action |
|---|---|
| Tap the weight | Load if unloaded |
| Tap anywhere while loaded | Unload |
| Hold the weight | Voltra auto load (pull and hold the cable, 3 s countdown) |
| Tap again on "CABLE OUT: TAP TO OVERRIDE" | Load at once with the cable pulled out |
| Swipe up / down | Weight +/- 1 lb, 5 lb steps when swiping fast |
| Gear button | Eccentric, chains, inverse chains, mountain |
| Tap the Bluetooth bar at the top | Connect screen: pick a Voltra, twin, un-twin |

The tick marks around the screen edge show the weight; they turn green while loaded.

## What you need

- Waveshare **ESP32-S3-Touch-AMOLED-2.06** (the ESP32-S3 version, not the C6), either:
  - [the watch on its own](https://www.amazon.ca/dp/B0FJFNXGNX), or
  - [the watch with a battery](https://www.amazon.ca/gp/product/B0FJFP6VLJ)
- For running unplugged, if your watch came without one: a 3.7 V LiPo with an MX1.25
  plug, such as [this one](https://www.amazon.ca/dp/B08215N9R8). **Its plug is wired the
  other way round and has to be re-pinned first** (see step 4).
- A USB-C data cable
- A Mac, Linux or Windows computer

## Load it onto a new watch

### 1. Install PlatformIO

Either the [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode),
or the command line:

```
brew install platformio        # macOS
pip install platformio         # anywhere with Python 3
```

### 2. Get the code

```
git clone https://github.com/stevenpoint/voltra_remote.git
cd voltra_remote
```

### 3. Flash the watch

Plug the watch in over USB-C, then:

```
pio run -e watch206 -t upload
```

The first build downloads the ESP32 toolchain and libraries, which takes a few minutes.
The watch restarts into the remote when the upload finishes.

If the upload cannot find the watch, hold the **BOOT** button while plugging it in, release
it once the port appears, and upload again.

Use only the `watch206` build on the watch. The `remote` build is for the knob and uses
different pins (GPIO 8 is the watch's display reset).

### 4. Fit the battery (optional)

**Check the polarity before plugging a battery in.** The red wire must go to the **+** pad
marked on the board. Aftermarket batteries with the same plug are sometimes wired the other
way round, including the one linked above; a reversed battery is not detected and the
watch will not run unplugged (and it risks damaging the board).

To swap a reversed plug: with a pin, gently lift the small plastic latch over each metal
contact on the plug and slide the wire out, then push the two wires back in on the
opposite sides until they click. Keep the bare contacts from touching each other while
they are out.

The watch charges the battery over USB-C. Its level shows at the bottom of the screen.

### 5. Connect to your Voltra

1. Close **Beyond+** on your phone first. The Voltra only keeps one controller.
2. On the watch, tap the Bluetooth bar at the top to open the Connect screen.
3. Tap your Voltra (`VTR-...`) in the list.

The watch remembers it and reconnects by itself next time. The watch never loads the
Voltra on its own: at start-up it only connects.

## Twin mode

Twin the Voltras **from the watch**, not from the Voltras' own screens: a hosting Voltra
stops advertising, so a remote can only drive the pair over a connection it made before
twinning (the way Beyond+ does it; see `docs/PROTOCOL.md`, "Twin mode").

1. Connect to the Voltra that should host.
2. Open the Connect screen and tap **Twin with VTR-...** under the other Voltra.
3. The top bar shows **TWIN**; weight, load, unload and the cable-out override now drive
   both, and the weight shows the pair's total.
4. **Un-twin** on the Connect screen undoes it.

If the watch loses the host while twinned it cannot reconnect until the pair is
un-twinned on the Voltras.

## Safety

- The watch only loads when you tap, hold or confirm the override. It never loads on
  start-up or after reconnecting.
- If Bluetooth drops, the watch shows it is disconnected and leaves the Voltra as it is.
- The cable-out override loads at once with the cable pulled out, skipping the Voltra's own
  hold-still check. Use it with a firm grip.
- Straps and printed parts are not a failsafe. Unload from the Voltra itself if anything
  feels wrong.

## Code layout

```
src/main.cpp              start-up
src/hw/                   display (display_watch206.cpp on the watch), touch, battery,
                          knob (swipes stand in for it on the watch)
src/ui/ui.cpp             screens; WATCH206 sections hold the watch layout
src/voltra/               BLE client and protocol (voltra_protocol.* is host-tested)
src/diag/flash_log.*      diagnostic builds: keeps the log in flash
docs/PROTOCOL.md          the Voltra protocol, including cable-out and twin captures
tools/                    font and icon generators, pull_log.py
```

Builds, from `platformio.ini`:

| Build | For |
|---|---|
| `watch206` | The watch |
| `watch206_diag` | The watch, logging all Voltra traffic to flash |
| `watch206_sweep` | As `watch206_diag`, plus a full register read every ~40 s |
| `remote` / `remote_diag` | The original knob |
| `native` | Protocol unit tests: `pio test -e native` |

To capture Voltra traffic away from a computer, flash `watch206_diag`, use the watch at
the Voltra, then plug it back in and run:

```
~/.platformio/penv/bin/python tools/pull_log.py
```

This saves the log to `voltra_capture.log`; `--clear` wipes it from the watch afterwards.
