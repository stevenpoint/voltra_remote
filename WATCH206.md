# Watch 2.06 port of kirby6365/voltra_remote

Do not `pio run -e remote -t upload` on this watch.

## Board

Waveshare ESP32-S3-Touch-AMOLED-2.06
- CO5300 QSPI AMOLED 410x502
- FT3168 touch @ 0x38, reset GPIO 9, INT GPIO 38
- I2C SDA 15 / SCL 14
- AXP2101 PMIC
- 32MB flash, 8MB OPI PSRAM
- No encoder, no DRV2605

## Pins (do not use Kirby 1.8 pins)

| Function | GPIO |
|---|---|
| LCD CS / SCLK / D0-D3 / RST / TE | 12 / 11 / 4,5,6,7 / 8 / 13 |
| I2C SDA / SCL | 15 / 14 |
| Touch INT / RST | 38 / 9 |

## First env (add to platformio.ini)

```
[env:watch206]
extends = env:remote
board_upload.flash_size = 32MB
board_upload.maximum_size = 33554432
board_build.partitions = default_16MB.csv
build_flags =
    ${common.build_flags}
    -DCORE_DEBUG_LEVEL=3
    -DWATCH206=1
    -DLCD_WIDTH=410
    -DLCD_HEIGHT=502
```

Flash with:

```
pio run -e watch206 -t upload
```

Only after display.cpp / touch are switched. Until then keep Hello World on the watch.

## Work order

1. Native tests — already PASS
2. New display path: Arduino_GFX CO5300 (known-good from Hello World) + LVGL 8 flush
3. Touch: pulse GPIO 9, then FT3168 @ 0x38 (06 example forgot the reset pin)
4. Stub knob_init() / knob events → swipe + tap
5. Stub haptics (no DRV2605)
6. Battery via AXP2101 later
7. Then turn on voltra::Client
