# CyberDeck — C3 + UNO Orchestration Build (Spec)

**Date:** 2026-11-12
**Boards:** ESP32-C3 (`v2/proto_readout_c3`) + Arduino UNO with 3.5" mcufriend TFT shield (`v2/proto_readout_uno`), soldered together.
**Goal:** Turn the existing passive WiFi-recon prototype into a multi-mode, Ghost-ESP-inspired toolkit driven from a native Android app, keeping the C3 as the single processing unit and the UNO as a dumb UART display.

This supersedes the single-board assumption in the original pasted plan (which put the TFT on the C3 over SPI). Here the display is on a separate board over a one-way UART, which is an advantage: **the display never contends with the radio.**

---

## 1. Decisions locked

| # | Decision | Choice |
|---|----------|--------|
| 1 | Firmware approach | **Path B** — our own orchestrator, using Ghost ESP only as a reference for what's possible. We do *not* flash Ghost ESP itself. |
| 2 | Capture/log storage | **No microSD** (none available, and the shield's SD is unreachable over the one-way link). **The phone is the disk** — the native app receives data over BLE and writes the files. |
| 3 | Android app | **Native** (Flutter + `flutter_blue_plus`, or Kotlin). Native is required because the app is also the storage backend + GPS source. |
| 4 | Alerts | **No piezo/buzzer** (dropped). Status is shown on the TFT, on the phone, and via LEDs. |
| 5 | LEDs | **Both on the UNO.** Green/passive = onboard `D13` LED (free). Red/active = one discrete LED on a spare UNO pin. |

---

## 2. Architecture

```
          ┌──────────────────────────┐          one-way UART           ┌───────────────────────┐
          │        ESP32-C3          │   GPIO21 ──────────────► D0      │   Arduino UNO         │
 phone ◄──┤  radio + brains + BLE    │   (19200 8N1, C3 → UNO)         │   3.5" mcufriend TFT  │
 (BLE)    │  ModeManager / handlers  │   common GND ──────────── GND    │   dumb renderer + LEDs│
          └──────────────────────────┘                                 └───────────────────────┘
```

- **C3** runs everything: the radio (WiFi promiscuous + raw 802.11 TX, or NimBLE), all tables/sorting, the mode state machine, and the BLE control link to the phone.
- **UNO** only paints rows the C3 sends it and drives two status LEDs. Holds no state (2 KB RAM).
- **Phone** is the control UI *and* the storage/GPS backend.

### The single-radio rule (shapes everything)
The C3 has one 2.4 GHz radio. It cannot reliably run WiFi and BLE at the same time (Bluedroid crashes it under coexistence — confirmed on this chip; NimBLE is used instead). So modes are grouped by radio role, and **crossing from a BLE role to a WiFi role tears the radio down and rebuilds it** (the "handoff"). Practical consequences:
- The BLE control link to the phone **drops whenever a WiFi-radio mode runs.** This is by design. The phone shows "running — reconnect to stop"; the UNO keeps showing live status the whole time.
- Because of the handoff, **capture and BLE-dump alternate** — the C3 captures to RAM, then reconnects BLE and ships the data to the phone. Fine for small targeted captures; impossible for continuous raw capture.

---

## 3. Feature set — what runs on the C3

Mapped to the Ghost ESP feature list. Only features that run on *this* hardware are in scope.

### ✅ Easy (several already built in the current sketch)
- **AP + station (client) scan** — built (`apTab`/`cliTab`, promiscuous).
- **Probe-request sniffing** — built ("following me" persistence list).
- **Packet monitor / live traffic graph** — built counters (`framesTotal`, `mgmtTotal`, `deauthTotal`); graph is UI on the phone/UNO.
- **Deauth detector** — built (`deauthTotal`).
- **Deauthentication (TX)** — `esp_wifi_80211_tx()` with a crafted deauth frame. *Own gear only — see §12.*
- **Beacon spam** — random / list / rickroll SSID flood via `esp_wifi_80211_tx()`.
- **BLE device scan** — NimBLE observer.
- **BLE advertisement spam** — Apple ("Sour Apple"), Android, Windows, Samsung pairing popups via NimBLE fast-cycling adverts.
- **Flipper Zero + AirTag/tracker detection** — NimBLE scan + classify by UUID/manufacturer data (reuses the tripwire classifier).

### 🟡 Works via the phone/RAM workaround (not native)
- **Handshake / PMKID capture** (vs the D-Link) — a handshake is only ~4 EAPOL frames (+ beacon), a few hundred bytes. Capture to C3 RAM, then on mode-exit reconnect BLE and stream to the phone, which assembles a real `.pcap`. See §7.
- **WiFi + BLE wardriving (Wigle)** — no GPS module, but the phone has GPS. C3 timestamps observations and batches them to the phone; the phone joins them to its own continuous GPS track by timestamp and writes the Wigle CSV. See §7.
- **Karma / probe-response tricks** — respond to parsed probe requests with crafted probe-responses (TX path). Fiddly.
- **Evil Portal / captive portal** — a *minimal* softAP + web server + DNS fits, but it's RAM-heavy and monopolizes the radio (no BLE while running). Keep it basic.

### ❌ Not on this hardware
- **Raw capture-everything PCAP** — no SD, can't stream live during capture (BLE is down), RAM too small. Only the targeted captures above work.
- **GPS-module wardriving** — no module; replaced by the phone workaround.
- **Cast to Chromecast / Roku / DIAL + smart-device control** — needs station-mode LAN join + SSDP/HTTP stack, conflicts with every scan/attack mode and eats the RAM budget. Stretch goal only, flashed in isolation if ever wanted; not part of the core build.

**Totals:** 8 easy, 4 via workaround, 3 out of scope.

---

## 4. C3 firmware architecture

Six loosely-coupled modules; nothing changes mode except `ModeManager.requestMode()`.

- **ModeManager** — the state machine, source of truth.
- **BleControl** — NimBLE GATT service for the phone (§6).
- **LinkDisplay** — composes per-mode screens and streams `R` rows to the UNO (generalizes the existing `setRow`/`composeAndSend`). Keep the 150 ms per-row pacing — that was the fix for the RX-overflow "bad lines" bug.
- **LedManager** — maps mode → LED command sent over the link (§5), since the LEDs physically live on the UNO.
- **mode_\*** handlers — one per mode below.
- **Capture/Logging** — RAM buffers + the BLE `DATA` characteristic (§7).

### Mode state machine

```c
enum Mode {
  PASSIVE,                                    // default — existing promiscuous recon (built)
  // --- WiFi-radio group (BLE control link is DOWN while these run) ---
  WIFI_SCAN, WIFI_DEAUTH, BEACON_SPAM,
  PROBE_FLOOD, KARMA, HANDSHAKE_CAPTURE, EVIL_PORTAL,
  // --- BLE-radio group ---
  BLE_SCAN, AIRTAG_SCAN, FLIPPER_SCAN, SKIMMER_SCAN,
  BLE_SPAM, BLE_HID_TRAP
};
```

- Each mode has `enter()` / `exit()`. `requestMode(next)` = `current.exit()` → **wait for clean radio teardown** (stop scans, deinit radio role, free buffers) → `next.enter()`. Never skip it — half-torn radio state is the C3's #1 flake source.
- **Boot → PASSIVE** unconditionally (green LED on the UNO).
- **Auto-revert to PASSIVE** after N minutes unless the phone refreshes — no active mode sits hot unattended.
- Control-link rule (revised 2026-11-12): on this single-radio C3, **BLE control and WiFi cannot run at once** (crash history). So the boot state is **`IDLE`** — BLE up, WiFi off, phone connects here. Selecting any **WiFi-group mode (including `PASSIVE` recon)** tears BLE down and runs WiFi; the UNO is the live surface; reconnect to stop. BLE-group stub modes (no radio yet) keep the control link up. `0xFF` = STOP → return to `IDLE`.

---

## 5. Link protocol (C3 → UNO)

Existing, keep as-is:
- `R<rr><c><text>\n` — draw row `rr` (00–18) in colour `c` (0–6) with `text`. 19200 8N1.

New commands to add:
- `C\n` — **clear screen.** Sent on every mode change so each mode starts from a clean layout.
- `L<g><r>\n` — **LED state.** `g`,`r` ∈ {`0`,`1`}. `g` = green/passive (onboard D13), `r` = red/active (discrete pin). e.g. `L10` = passive, `L01` = active.

The UNO ignores any line it doesn't recognise, so old `R` handling is untouched.

---

## 6. BLE control protocol (C3 ↔ phone)

**Stack:** NimBLE-Arduino (not Bluedroid — it crashes this C3).

**Service** (custom 128-bit UUID), three characteristics:

| Char | Props | Payload |
|------|-------|---------|
| `CMD` | Write (bonded) | `byte[0]` = opcode (one per mode), `byte[1..]` = params: target channel / target BSSID / SSID-list index / spam flavour (0=Apple,1=Android,2=Windows,3=Samsung) |
| `STATUS` | Notify | current mode + short status string (same text the UNO shows) |
| `DATA` | Notify | chunked binary — `.pcap` frames and wardrive batches (§7) |

- **Security:** require bonding with a passkey so only your phone pairs. Bond-based trust (not MAC matching — phones randomize).
- **Clock sync:** on connect the phone writes the current epoch to `CMD` (dedicated opcode) so the C3's timestamps align to the phone's GPS log.
- **Handoff behaviour:** `STATUS` can't notify while a WiFi mode runs (BLE is down). During those modes the **UNO is the live status surface**; the phone shows "running WIFI_x — reconnect to stop."

### Opcode table (draft)
`0x00` PASSIVE · `0x01` WIFI_SCAN · `0x02` WIFI_DEAUTH · `0x03` BEACON_SPAM · `0x04` PROBE_FLOOD · `0x05` KARMA · `0x06` HANDSHAKE_CAPTURE · `0x07` EVIL_PORTAL · `0x10` BLE_SCAN · `0x11` AIRTAG_SCAN · `0x12` FLIPPER_SCAN · `0x13` SKIMMER_SCAN · `0x14` BLE_SPAM · `0x15` BLE_HID_TRAP · `0xF0` CLOCK_SYNC · `0xF1` PULL_CAPTURE (drain RAM buffer to `DATA`).

---

## 7. Capture & logging via the phone (the no-SD solution)

Three flows, all shaped by the single-radio handoff (capture and dump alternate, never simultaneous):

1. **Handshake / PMKID** — promiscuous with an EAPOL/PMKID filter (extends the existing `onPacket()`), channel-locked, trigger a reconnect. Buffer the frames in RAM (few hundred bytes). On mode-exit, BLE comes back; phone pulls them via `DATA` and writes a `.pcap`.
2. **Wardriving (WiFi + BLE)** — C3 timestamps each AP/BLE observation, batches in RAM (~12 KB, a few hundred records). Time-slice back to BLE to dump batches. Phone logs its own GPS track continuously and **joins by timestamp** → Wigle CSV. Requires the CLOCK_SYNC on connect.
3. **Live stats / packet-monitor graph** — streamed over `STATUS` whenever BLE is up; phone draws the graph and keeps the timeline.

Hard limit to remember: this works for *small targeted* captures only. Continuous raw PCAP is out (no SD, RAM too small, BLE down during capture).

---

## 8. UNO firmware changes (`orchestrator/v1/uno/uno.ino`)

Minimal — it stays a dumb renderer:
1. Handle the new `C\n` (clear screen) command.
2. Handle the new `L<g><r>\n` (LED) command.
3. Drive the two LEDs:
   - **Green / passive → onboard `D13`** (free, no wiring; SD is unused so `D13`/SCK is available).
   - **Red / active → one discrete LED** on a spare pin (`A5`, or `D11`/`D12` — all free with SD unused) via a ~330 Ω resistor to GND.
4. Keep literal printf widths (`%-40.40s`) — AVR printf silently drops the `*` dynamic-width specifier.

**Access caveat:** if the mcufriend shield covers the UNO headers (most do, no stacking headers), wiring the discrete red LED means tapping a pin from the underside or a header pass-through. **Fallback if that's impractical:** use the onboard `D13` LED alone as the critical indicator — **off = passive, solid = any active mode.** One LED carries the safety-relevant bit (am I transmitting) with zero wiring. The green/red pair is the nicer version; the single onboard LED is the guaranteed-to-work version.

**Honesty note:** an LED on the UNO reflects the *last command the C3 sent*, not the C3's live state — if the link drops it goes stale. The on-screen status already shows mode + link health, so this is acceptable; just know the red LED is "best effort," not gospel.

---

## 9. Android app (native)

Flutter + `flutter_blue_plus` (or Kotlin). Responsibilities:
- **BLE client:** bond (passkey) → write `CMD` → subscribe `STATUS` + `DATA`.
- **Control UI:** a **mode grid** (all §4 modes), mutually exclusive; per-mode param inputs (channel / BSSID / SSID-list / spam flavour); a status panel.
- **Disconnect handling:** treat the drop when a WiFi/trap mode starts as expected — show "running, reconnect to stop," not an error.
- **Storage backend:** assemble `.pcap` from `DATA`; write Wigle CSV; keep logs.
- **GPS source:** log a continuous GPS track; send CLOCK_SYNC on connect; join wardrive batches to fixes by timestamp.

---

## 10. Build sequence (milestones)

1. **Skeleton** — `ModeManager` + `LinkDisplay` + `LedManager` reflecting state, toggled over the C3's USB serial. UNO already renders. Prove transitions + the `C`/`L` commands + LEDs work. *(Half-done: passive recon exists.)*
2. **BLE control** — NimBLE GATT + bonding; drive modes from nRF Connect before writing the app.
3. **Passive** — existing recon loop behind the state machine.
4. **BLE group** — scan / airtag / flipper / skimmer (no TX), then spam, then HID trap.
5. **WiFi group** — deauth / beacon / probe / karma against **your own D-Link**; handshake capture last (RAM→phone).
6. **Phone app** — built last, once the opcode protocol is frozen.

Test each stage in isolation; don't stack unproven radio layers.

---

## 11. Flashing procedure (boards powered separately, soldered link)

- **Flash the C3 anytime.** It flashes over its own USB (native USB-JTAG), which doesn't use `GPIO21`. The UNO doesn't interfere (one-way link).
- **Flash the UNO: power the C3 down first.** The C3's constant bytes into `D0` corrupt the UNO bootloader; unpowering the C3 silences it so the upload succeeds. Power the C3 back up promptly afterward.
  - *Electrical note:* during the UNO upload its USB-serial drives `D0` to 5 V, which leaks into the unpowered C3's `GPIO21` via its protection diode — low current, fine for occasional use, don't leave it sitting. No wire-cut needed.

---

## 12. Constraints & gotchas

- **UART is display-only** — no packet data over it; capture goes to the phone, not the UNO.
- **Single radio** — BLE control drops during WiFi modes (handoff); the UNO covers status meanwhile.
- **Teardown cleanliness** — enforce in every `exit()`.
- **RAM** — NimBLE + tables + TX/capture buffers on ~400 KB; prefer static buffers.
- **C3 is Ghost ESP's weakest target** — expect to trim.
- **MAC randomization** — bond-based phone trust, not MAC matching.
- **HID layout** — defaults to US; special chars in the ducky script may remap.
- **Level shifting** — the one-way C3 (3.3 V) → UNO (5 V) direction is safe (UNO reads 3.3 V as HIGH). Never wire the reverse without a divider/level shifter.
- **Legal / scope:** deauth, beacon/probe/BLE spam, karma and HID are active TX. Run them **only against your own gear in your own lab** — the own-lab scope set on 2026-11-10. Not against others' devices.

---

## 13. Physical change summary

- **C3:** no physical changes. (LEDs moved to the UNO, piezo dropped, no SD.)
- **UNO:** add one discrete red LED on a spare pin (`A5`/`D11`/`D12`) + ~330 Ω to GND for the active/TX indicator; green/passive uses the onboard `D13` LED (free). Fallback: onboard `D13` alone (off = passive, on = active) if header access under the shield is impractical.
- **Link:** unchanged — `GPIO21 → D0`, common GND, 19200 8N1, one-way.
