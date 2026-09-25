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
| Connect screen: "Twin with ..." | Twin two Voltras (see Twin mode) |

Close **Beyond+** before connecting. The trainer only keeps one controller.

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-2.06 (S3, **not** C6)
- Optional: EEMB 402535 320 mAh MX1.25 pouch in the back
- Strap the watch to the bar so the screen faces you

## Friday bring-up (do this first)

Do **not** flash Voltra firmware until the stock panel works.

1. Install Arduino IDE or `arduino-cli` + esp32 core **3.3.x**.
2. Clone Waveshare examples:
   `git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06.git`
3. Open `examples/arduino/01_HelloWorld` and `06_LVGL_Arduino_v9`.
4. Board: **ESP32S3 Dev Module**, USB CDC on Boot = Enabled, PSRAM = OPI PSRAM, Flash 16 MB or whatever the listing says (this SKU is 32 MB on many units — if flash fails, try 16 MB).
5. Hold **BOOT**, plug USB-C, release BOOT after the port appears if the Mac/PC does not see it.
6. Confirm: white screen + Hello World, then LVGL widgets + touch.

Only after that, open `firmware/VoltraWatch`.

## Firmware layout

```
firmware/VoltraWatch/
  VoltraWatch.ino     // setup / loop
  pin_config.h        // 2.06 GPIO map
  gestures.h/.cpp     // tap / swipe
  ui.h/.cpp           // big weight + state
  link.h/.cpp         // BLE client — copy protocol from voltra-knob
```

`link.cpp` is a stub until we drop in Omar Shahine’s `src/protocol` + `src/link` from
https://github.com/omarshahine/voltra-knob

Those files are host-tested C++ and do not care about the encoder. We keep his frames and swap the board/UI layer.

## Safety

- Never auto-load on boot.
- Cap live weight changes (default 25 lb while loaded).
- If BLE drops mid-set, do not guess; show DISCONNECTED and leave the trainer as-is.
- Printed straps and plastic are not a failsafe. Unload from the Voltra face if anything feels wrong.

## Twin mode

Twin the Voltras **from the remote**, not from the Voltras' own screens: a hosting Voltra
stops advertising, so a remote can only drive the pair over a connection it made before
twinning (the way Beyond+ does it; see `docs/PROTOCOL.md`, "Twin mode").

1. Connect to the Voltra that should host.
2. Open the Connect screen and tap **Twin with VTR-...** under the other Voltra.
3. The top bar shows **TWIN**; weight, load and unload now drive both.
4. **Un-twin** on the Connect screen undoes it.

If the remote loses the host while twinned it cannot reconnect until the pair is
un-twinned on the Voltras.
