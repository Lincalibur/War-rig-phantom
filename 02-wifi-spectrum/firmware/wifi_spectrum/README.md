# wifi_spectrum — flashing notes

## One-time setup in Arduino IDE
1. File > Preferences > Additional Board Manager URLs, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Tools > Board > Boards Manager > install **esp32** (by Espressif Systems).
3. Tools > Manage Libraries > install **Adafruit SSD1306** and **Adafruit GFX Library**.
   (NimBLE-Arduino is no longer needed for this sketch.)

## Flash
1. Open `wifi_spectrum.ino` in Arduino IDE.
2. Plug in ESP32-C3 #2 over USB.
3. Tools > Board > esp32 > **ESP32C3 Dev Module**.
4. Tools > Port > pick the COM port that appeared when you plugged it in.
5. **Important:** Tools > USB CDC On Boot > **Enabled**, or `Serial` output goes to the UART pins instead of the USB port. Via `arduino-cli`: `--fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc'`.
6. Upload.

## Confirm it worked
1. Tools > Serial Monitor, baud **115200** — should print `WiFi spectrum node — channel congestion sweep` at boot.
2. The OLED should start showing a bar graph labeled "WiFi spectrum (ch 1-13)" within a few seconds, updating roughly every 2.6s (one full 13-channel sweep).
3. On the hub, the Spectrum screen (screen 3) should go from flat to showing the same shape once ESP-NOW reports start arriving.
