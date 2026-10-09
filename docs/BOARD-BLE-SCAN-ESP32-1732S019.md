# BLE-SCAN board: ESP32-1732S019

**Deck name: `BLE-SCAN`.** This is the classic ESP32 board with the built-in 1.9" colour screen, bought from nde3d. In the deck it is the Scan 2 module: BLE tracker detection on its own screen, reporting to the `HUB` over UART. All board names are listed in [`hardware-layout.md`](hardware-layout.md#board-names).

Earlier notes also call this board "the ESP-32S with the attached screen" and "the prototype hub". It is the same board. It is **not** the hub any more; the hub is the ESP32-S3.

Source: nde3d's product sheet, kept as [`ESP32-1732S019_Datasheet.pdf`](ESP32-1732S019_Datasheet.pdf). The sections marked "deck" are this project's own choices.

## Specifications

| Item | Value |
|---|---|
| Microcontroller | ESP32-D0WD-V3, dual-core Xtensa LX6, up to 240 MHz |
| Flash | 4 MB |
| PSRAM | None |
| WiFi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | Classic (BR/EDR) and BLE |
| Display | 1.9" IPS TFT, 170 × 320 px, non-touch |
| Display driver | ST7789, 4-wire SPI |
| USB | Type-C, CH340 USB-to-serial |
| Logic level | 3.3 V |
| Power input | 5 V via USB-C |
| Onboard extras | BOOT and EN buttons only. No SD slot |

This is the classic ESP32 version of the board, not the ESP32-S3 version.

## Display pins (fixed on the board)

| Signal | GPIO |
|---|---|
| SCLK | 14 |
| MOSI | 13 |
| MISO | not connected |
| CS | 15 |
| DC | 2 |
| RST | tied to EN (use -1 in software) |
| Backlight | 21 |

- **Backlight:** drive GPIO 21 high with a plain `digitalWrite`. Don't use PWM; it has a known issue on recent ESP32 cores.
- **Free GPIOs:** 4, 16, 17, 22, 25, 26, 27, 32, 33.
- **Never use GPIO 6–11:** they are the onboard flash.

## Deck wiring (planned)

| Function | GPIO | Connects to |
|---|---|---|
| Link TX | 17 | `HUB` BLE_LINK_RX (S3 GPIO1), through ~1 kΩ |
| Link RX | 16 | `HUB` BLE_LINK_TX (S3 GPIO2), through ~1 kΩ |
| GPS RX | 22 | NEO-7M TX (shared with `WIFI-NODE`) |
| 5 V | 5 V pin | Section switch 3 |
| GND | GND | Common ground |

GPIO 16/17 are the ESP32's default UART2 pins. The remaining free GPIOs (4, 25, 26, 27, 32, 33) are spare for a local alert LED, buzzer or button. These assignments are a plan and have not been wired or tested.

## Arduino build settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module (`esp32:esp32:esp32`) |
| Flash size | 4 MB |
| Upload speed | 921600; drop to 115200 if uploads fail |
| Driver | CH340 |

If an upload fails with "Failed to connect / wrong boot mode": hold BOOT, tap EN, release BOOT, then upload again.

## Display library configuration

nde3d recommends LovyanGFX. The whole pin configuration lives in the sketch.

```cpp
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host   = SPI2_HOST;   // HSPI
      c.spi_mode   = 0;
      c.freq_write = 40000000;
      c.pin_sclk   = 14;
      c.pin_mosi   = 13;
      c.pin_miso   = -1;
      c.pin_dc     = 2;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs   = 15;
      c.pin_rst  = -1;            // reset tied to EN
      c.panel_width  = 170;
      c.panel_height = 320;
      c.offset_x = 35;            // 170-wide panel offset
      c.offset_y = 0;
      c.invert   = true;          // IPS panel
      c.rgb_order = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};
LGFX tft;

void setup() {
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);         // backlight on
  tft.init();
  tft.setRotation(1);             // landscape, 320 x 170
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Hello!", 10, 10);
}
void loop() {}
```

- If red and blue look swapped, change `rgb_order`.
- If the image is shifted sideways, adjust `offset_x`.

## Limits that matter for the deck

- **4 MB flash, no PSRAM:** keep device tables small, and prefer NimBLE over Bluedroid to save flash and RAM.
- **No SD slot:** it cannot log on its own; logging happens on the `HUB`.
- **No touch and no user buttons:** any input needs a button on a free GPIO.
- **Board outline is not in the product sheet.** Measure it before cutting the 55 × 40 mm lid zone.
