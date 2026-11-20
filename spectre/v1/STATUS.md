# SPECTRE — project status & handoff

**Last updated:** 2026-11-15 · **Branch:** `main` · **Repo:** Lincalibur/War-rig-phantom

> This file is the cross-machine handoff. The Windows PC does the editing/compiling; the
> **ESP32 board is on the Kubuntu laptop** (`/dev/ttyUSB0`, Claude CLI installed there too).
> Only the git repo syncs between the two machines — so this file is the source of truth.

---

## What SPECTRE is
The **SPECTRE HUB** = the CyberDeck **orchestrator** firmware ported onto a single
**ESP32-WROOM-32U** (external antenna) with a **GPS** and a **3.5" TFT**. It replaces the old
two-chip orchestrator (soldered ESP32-C3 brain + Arduino UNO readout). The freed C3+UNO pair
becomes **SENTINEL** (passive counter-surveillance watcher) later.

> ⚠️ cifertech's **ESP32-DIV was only a reference example** ("what's possible"), NOT the
> codebase. Any `...\ESP32-DIV\...` edits are dead. The real code is `spectre/v1/`.

## Hardware (confirmed)
- Controller: **ILI9488** (RDID 0xD3 → `94 88`), 3.5" **8-bit parallel** (mcufriend-style shield), **320x480**.
- 3.3V logic rail fix done: ESP32 **3V3 → shield 3.3V pad** wired (shield doesn't self-regulate). Backlight hardwired to VCC.
- TFT pinout (see `User_Setup_SPECTRE.h`) — optimized SPECTRE-carrier mapping 2026-11-15:
  CS=27, DC/RS=26, RST=14, WR=25, RD=33; D0-D7 = 22,23,16,17,5,18,19,21.
- GPS: NEO-7M on **UART2**, RX=15 / TX=2 (separate from the 2.4GHz radio, always on). Expansion SPI (future radio) = SCK 13 / MOSI 12 / MISO 32 / CS 4 — not used in firmware yet.
- Strapping pins to watch if boot/flash flakes: GPIO12 (EXP_MOSI, fit R1 pulldown), GPIO2/15 (GPS UART), GPIO5 (D4).

## Code state — DONE, compiles, hardware-untested
- `spectre/v1/spectre/spectre.ino` (+ `ducky.h`): orchestrator on one WROOM. TFT_eSPI direct draw
  (portrait, `setRotation(0)`, shadow-diff), GPS NMEA GGA+GSV parser, single-radio handoff
  (BLE drops during WiFi modes; GPS always on). Compiles ~34% flash / 22% RAM.
- Modes carried over from orchestrator + **new** this pass (user: add all except evil-portal
  enhancement; keep current evil portal; remove nothing):
  - Wardrive `0x08` (live GPS-stamped APs on TFT, **no logging** — app is just the instigator)
  - Satellite Scan `0x09` (GPS-only, BLE stays up)
  - Packet Monitor waterfall `0x0A`
  - BLE Sniffer `0x16` (tracking table), BLE Jammer `0x17` (best-effort adv-flood, honestly labelled)
  - Richer WiFi scan (OUI vendor + enc type)
  - **Handshake → pcap finished**: raw EAPOL buffered in RAM, streamed base64 over the DATA
    characteristic on `PULL_CAPTURE 0xF1`; phone decodes + saves LINKTYPE_IEEE802_11 .pcap.
- App `app/lib/main.dart`: 5 new mode tiles + briefings, GPS section, pcap pull/decode/save
  (`path_provider` added to pubspec). `flutter analyze` error-free. APK built on Windows at
  `C:\Users\liamo\Desktop\CyberDeck.apk` (48.3MB).

## ⚠️ PINOUT REMAPPED (2026-11-15) — prebuilt bins are STALE
TFT + GPS pins were remapped to the optimized SPECTRE-carrier layout (see the pinout line above).
**`spectre/v1/prebuilt/*_MERGED.bin` were built with the OLD pins — do NOT flash them** or the
display/GPS won't work. Rebuild from source (`spectre/v1/spectre/spectre.ino` + the updated
`User_Setup_SPECTRE.h`) and regenerate the merged bin before flashing.

## ✅ DISPLAY SOLVED (2026-11-15)
The whole white-screen saga is done. Final state, working on hardware:
- Root cause 1 (stuck on old build): config never reached the board because editing/compiling is
  on Windows but flashing is from Kubuntu. **Fix = prebuilt merged binary** flashed at `0x0`
  (Kubuntu Arduino/TFT_eSPI setup irrelevant). `spectre/v1/prebuilt/*_MERGED.bin`.
- Root cause 2 (tiny text): firmware was laid out for 240x320; scaled to the real 320x480 panel —
  `ROW_H=24`, `TEXT_SIZE=2`, 20 rows × 24 = 480 full height.
- Root cause 3 (white background): **ILI9488 inverts by default** → `tft.invertDisplay(true)` after
  `tft.init()`. Black bg + coloured text now correct.
- 3.3V logic rail fix (ESP32 3V3 → shield 3.3V pad) + wiring were already sorted by the user.

**Flash the main firmware (board on `/dev/ttyUSB0`):**
```bash
git pull
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 spectre/v1/prebuilt/spectre_MERGED.bin
```
`tft_diag_MERGED.bin` is still there as a display sanity-check if ever needed.

## ▶ NEXT STEPS — hardware-test the modes
Display works; now validate behaviour, driven from the CyberDeck Android app over BLE.
Suggested order: passive first (WiFi scan, BLE scan, GPS/Satellite, Wardrive, Packet Monitor),
then handshake→pcap pull-to-phone, then the active-TX modes last.
**Deauth/beacon/probe/karma/BLE spam/jammer/HID are active TX — own lab / own gear only**
(scope affirmed; homelab ready). Deauth requires picking a target in the app.
APK already built at `C:\Users\liamo\Desktop\CyberDeck.apk`; rebuild if the app changed:
`C:\Users\liamo\flutter\bin\flutter.bat build apk --release`.

## Build toolchain (if compiling spectre.ino on Kubuntu)
ESP32 Dev Module, **Huge APP** (3MB/1MB), 4MB, PSRAM off; core **esp32:esp32 2.0.17**,
**NimBLE-Arduino 2.5.1**, **TFT_eSPI 2.5.43** with `User_Setup_SPECTRE.h` copied over
`libraries/TFT_eSPI/User_Setup.h`, and **`-Wl,-zmuldefs`** via `platform.local.txt` (raw 802.11 TX).
Open item: deauth raw-TX may need the `ieee80211_raw_frame_sanity_check` override on core 2.0.x.

> Note: the Windows PC's Claude memory (`.claude/...`) does NOT sync here — this file does.
