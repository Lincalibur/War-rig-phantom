# Step 2.1 — raw BLE scan (first bring-up sketch)

Tests: "confirm raw scan lists nearby devices with sane RSSI" (Component 2 checklist).

## One-time setup in Arduino IDE
1. File > Preferences > Additional Board Manager URLs, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Tools > Board > Boards Manager > install **esp32** (by Espressif Systems).
3. Tools > Manage Libraries > install **NimBLE-Arduino** (by h2zero).

## Flash
1. Open `ble_scan.ino` in Arduino IDE.
2. Plug in ESP32-C3 #2 over USB.
3. Tools > Board > esp32 > **ESP32C3 Dev Module**.
4. Tools > Port > pick the COM port that appeared when you plugged it in.
5. Upload. If upload fails to start, some C3 boards need you to hold **BOOT**, tap **RESET**, release **BOOT** right as upload begins — board-dependent, only needed if it doesn't upload normally.

## Confirm it worked
1. Tools > Serial Monitor, baud **115200**.
2. You should see `--- scanning ---` followed by lines like:
   ```
   mac=aa:bb:cc:dd:ee:ff  rssi=-62  name="Liam's iPhone"
   ```
3. Walk your phone (with Bluetooth on) near the board and confirm it shows up with a plausible RSSI (closer = less negative, roughly -40 to -60; farther/through walls = more negative, -80 and beyond).

## If nothing shows up
- Confirm the right board variant is selected (some C3 boards need "USB CDC On Boot: Enabled" under Tools if the native USB port doubles as serial — check Tools menu for that option and enable it if present, then re-upload).
- Confirm Bluetooth is actually on for whatever phone/device you're testing with.

Once this is confirmed working, next step is 2.2 (time-bucketed MAC/RSSI log) — say the word and I'll write that sketch on top of this one.
