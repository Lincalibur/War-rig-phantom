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
- TFT pinout (see `User_Setup_SPECTRE.h`): CS=33, DC/RS=15, RST=32, WR=4, RD=2; D0-D7 = 12,13,26,25,17,16,27,14.
- GPS: NEO-7M on **UART2**, RX=21 / TX=22 (separate from the 2.4GHz radio, always on). SD lines (18/19/23/5) NOT wired — display only for now.
- Strapping pins to watch if boot/flash flakes: GPIO12 (D0), GPIO2 (RD).

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

## ⛔ The blocker and the fix
**White screen saga root cause:** all edits/compiles happened on Windows, but the board is
flashed from Kubuntu — the correct TFT config **never reached the board** (it kept booting the
old 240x320 build). Hardware was fine the whole time.

**Fix (shipped):** a **pre-compiled merged binary** with the correct ILI9488/320x480/parallel
config baked in, so the Kubuntu Arduino/TFT_eSPI setup is irrelevant.
→ `spectre/v1/prebuilt/tft_diag_MERGED.bin`, flashed at `0x0`. Steps in `prebuilt/README.md`.

## ▶ NEXT STEPS (run on the Kubuntu laptop)
1. `git pull` (branch `main`).
2. Flash the diagnostic (board on `/dev/ttyUSB0`, 3V3 wire connected):
   ```bash
   pip install esptool   # once, if needed
   esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 spectre/v1/prebuilt/tft_diag_MERGED.bin
   ```
   (or web flasher `espressif.github.io/esptool-js`, file @ `0x0`).
   **Success** = Serial @115200 shows `width=320 height=480`, `94 88`, and the panel **cycles
   RED/GREEN/BLUE/WHITE/BLACK**.
3. If colours work → build the **main `spectre.ino`** into a merged `.bin` the same way and flash it.
4. Hardware-test all modes. **Deauth/beacon/probe/karma/BLE spam/jammer/HID are active TX —
   own lab / own gear only** (scope affirmed by user; homelab is ready). Targeting system stays
   mandatory for deauth.
5. APK already built (see above) — rebuild if the app changed:
   `C:\Users\liamo\flutter\bin\flutter.bat build apk --release`.

## Build toolchain (if compiling spectre.ino on Kubuntu)
ESP32 Dev Module, **Huge APP** (3MB/1MB), 4MB, PSRAM off; core **esp32:esp32 2.0.17**,
**NimBLE-Arduino 2.5.1**, **TFT_eSPI 2.5.43** with `User_Setup_SPECTRE.h` copied over
`libraries/TFT_eSPI/User_Setup.h`, and **`-Wl,-zmuldefs`** via `platform.local.txt` (raw 802.11 TX).
Open item: deauth raw-TX may need the `ieee80211_raw_frame_sanity_check` override on core 2.0.x.

> Note: the Windows PC's Claude memory (`.claude/...`) does NOT sync here — this file does.
