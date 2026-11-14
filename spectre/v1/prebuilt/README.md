# SPECTRE prebuilt binaries — flash from Kubuntu

These are **pre-compiled** on the Windows PC with the correct **ILI9488 / 320x480 / 8-bit parallel**
TFT_eSPI config already baked in. Flashing one of these makes the Kubuntu machine's Arduino +
TFT_eSPI setup **irrelevant** — no `User_Setup.h`, no library versions, no cache to fight.

## `tft_diag_MERGED.bin` — the display bring-up diagnostic (flash this FIRST)

Single merged image (bootloader@0x1000 + partitions@0x8000 + boot_app0@0xe000 + app@0x10000),
so it flashes at offset **0x0**.

### Flash it (board on `/dev/ttyUSB0`, keep the ESP32 3V3 -> shield 3.3V wire connected)

Terminal:
```bash
pip install esptool                 # once, if not installed
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 tft_diag_MERGED.bin
```
(If `esptool.py` isn't found: `python3 -m esptool ...`)

Or browser (no terminal): open `https://espressif.github.io/esptool-js/` in Chrome/Chromium,
**Connect** -> `/dev/ttyUSB0`, set the file row to Flash Address **0x0**, pick this `.bin`, **Program**.

### Expected result (Serial @115200)
- `[diag] ESP32 booted` and `width=320 height=480`  <- proves the right config is now on the board
- `RDID 0xD3 -> ... 94 88`  (ILI9488 confirmed)
- panel **cycles RED / GREEN / BLUE / WHITE / BLACK** with a label

If that works, the main SPECTRE firmware gets built into a merged `.bin` the same way.

## Component binaries (if you prefer offset flashing instead of the merged image)
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash \
  0x1000  tft_diag.ino.bootloader.bin \
  0x8000  tft_diag.ino.partitions.bin \
  0x10000 tft_diag.ino.bin
# boot_app0.bin @ 0xe000 comes from the esp32 core (tools/partitions/boot_app0.bin)
```
