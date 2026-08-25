# OPSEC/OSINT Deck — Implementation Plan

**Scope note:** every module below is passive listening / logging on your own gear. Nothing here transmits deauth frames, injects packets, or touches networks you don't own. Keep it that way — it keeps you legal and keeps the deck usable in more places.

## Hardware inventory → role assignment

| Device | Role | Why |
|---|---|---|
| ESP32-C3 #1 | WiFi sniffer node | Native promiscuous-mode 802.11 access via ESP-IDF |
| ESP32-C3 #2 | BLE hunter node | NimBLE stack, dedicated to BLE scan/track |
| Arduino Uno | Sub-GHz node + control surface | No radio, so gets the one job that doesn't need one, plus physical I/O |
| ESP32 + screen | Hub console | Only device with a display already wired — becomes the aggregator/dashboard |
| Small LCD | Attached to BLE hunter | Portable handheld tracker-hunt readout |
| Big LCD | Attached to Arduino | Deck's main status board / menu display |

---

## Module 1 — WiFi Sniffer Node (ESP32-C3 #1)

**Purpose:** passively capture 802.11 management frames (beacons, probe requests/responses) to map nearby APs and detect devices leaking their preferred-network list.

**Toolchain:** ESP-IDF, not Arduino core. Arduino's WiFi library doesn't expose promiscuous-mode callbacks with full frame headers — you need `esp_wifi_set_promiscuous(true)` and `esp_wifi_set_promiscuous_rx_cb()` directly.

**Core firmware pieces:**
1. **Promiscuous mode init** — set WiFi to `WIFI_MODE_NULL`/station-idle, register the RX callback, enable promiscuous mode.
2. **Channel hopper** — cycle through channels 1–13 on a timer (e.g. every 300ms) so you're not stuck listening to one channel. Track hop timing so you can log which channel each frame came from.
3. **Frame filter** — in the RX callback, check `wifi_promiscuous_pkt_type_t`: keep `WIFI_PKT_MGMT`, discard data/control frames initially (you can widen this later).
4. **802.11 header parsing** — pull frame subtype (beacon = 0x80, probe req = 0x40, probe resp = 0x50), source/dest/BSSID MACs, and for beacons/probe-resp walk the tagged parameters to extract the SSID (tag number 0).
5. **Dedup/aggregation layer** — a small hash table (BSSID+SSID key) so you're not spamming duplicate beacon logs every 100ms; track first-seen, last-seen, RSSI min/max, and a hit count instead.
6. **Deauth/disassoc counter** — flag reason-code frames (subtype 0x0C/0x0A) as a simple attack-awareness signal — don't act on them, just count and log spikes.

**Data record to emit per unique entity:**
```
{ ts, mac, ssid, rssi, channel, frame_type, hits }
```

**Standalone test milestones:**
- Serial-print every beacon seen near you, confirm SSID/RSSI look right.
- Confirm channel hopping actually covers all 13 channels (log channel alongside each hit).
- Let it run 10 minutes in a busy area, check the dedup table doesn't blow up memory (C3 has ~400KB usable RAM — cap table size, evict oldest on overflow).
- Validate probe-request capture separately — this is the privacy-relevant one (phones broadcasting known SSIDs while idle).

**No local display** — this node just needs a status LED (scanning / hop-active) and serial-console output for standalone debugging. All persistent output goes to the hub once integrated.

---

## Module 2 — BLE Hunter Node (ESP32-C3 #2 + small LCD)

**Purpose:** scan BLE advertisements continuously, and specifically flag devices that keep reappearing across time/location — the signature of an unwanted tracker (AirTag/Tile/SmartTag) rather than a stationary BLE beacon.

**Toolchain:** NimBLE-Arduino (lighter than the Bluedroid stack, fits C3 RAM comfortably better than raw ESP-IDF BLE for this).

**Core firmware pieces:**
1. **Active BLE scan loop** — continuous scan, short scan windows (e.g. scan every 5s for 3s) to balance coverage vs power.
2. **MAC/RSSI log with time buckets** — record `(mac, rssi, timestamp)` per advertisement. Group timestamps into coarse "sighting windows" (e.g. 10-minute buckets).
3. **Persistence scoring** — the actual tracker-hunt logic: a MAC is "suspicious" if it appears in N or more distinct sighting windows across a configurable span (e.g. seen in 3+ windows over the last hour) *while you've been moving* — this is the part that distinguishes "my neighbor's smart bulb" from "something following me."
4. **Rotating-MAC awareness** — note in the UI that modern trackers (AirTag) rotate their BLE address periodically, so pure MAC-matching will miss some. A secondary heuristic: match on manufacturer-specific data patterns in the advertisement payload (Apple's Find My payload has a recognizable format) rather than MAC alone.
5. **RSSI "getting warmer" mode** — once a device is flagged, switch to a fast-poll direction-finding mode: sample RSSI at high rate and show a live bar/needle on the small LCD so you can physically walk it down.

**Small LCD wiring (assume a small SPI/I2C module — confirm exact part when you get here):**
- I2C OLED (SSD1306, common 0.96"): SDA/SCL + power, minimal pin count, good for list views.
- SPI TFT (ST7735/ST7789, common small color LCD): more pins but supports a real RSSI bar graph nicely.
- Decide based on what you actually have — flag the exact model when we get to wiring and I'll give exact pin mapping and driver library.

**Standalone test milestones:**
- Confirm raw BLE scan lists nearby devices with sane RSSI.
- Walk around with a known BLE device (your phone) and confirm persistence scoring flags it correctly without flagging your own stationary devices at home.
- Validate the small LCD renders a live-updating RSSI readout without flickering/blocking the scan loop (use a non-blocking display update, not `delay()`).

---

## Module 3 — Sub-GHz + Control Surface (Arduino Uno + big LCD)

**Purpose:** two jobs bundled onto the one device with no radio of its own — passive 433/315MHz listening, and acting as the deck's physical control panel/status board.

**Sub-GHz hardware:** a cheap superheterodyne OOK/ASK receiver module (RXB6, XY-MK-5V, or similar — avoid the super-cheap regen receivers, they're noisy). Antenna: ~17.3cm wire, quarter-wave for 433MHz.

**Sub-GHz firmware:**
1. Use the `rc-switch` library as a starting point — it decodes a large set of common fixed-code OOK protocols (garage remotes, cheap doorbells, some weather stations) out of the box.
2. For anything `rc-switch` doesn't recognize, fall back to raw pulse-timing capture on an interrupt pin, logging pulse widths — you can eyeball/post-process these to reverse-engineer unknown protocols later.
3. Log: `{ ts, protocol_or_raw, code, pulse_length, repeat_count }`.

**Control-surface hardware:** rotary encoder or 3–4 momentary buttons, wired to remaining digital pins.

**Big LCD wiring:** depends on what you have — parallel character LCD (HD44780 16x2/20x4, needs 6+ data pins or an I2C backpack — get the backpack if you don't already have one, it frees up pins) vs a larger graphic TFT (more capable but eats more of the Uno's limited RAM — the Uno only has 2KB SRAM, so a big color TFT with a frame buffer is tight; a character LCD or a small graphic LCD with page-buffered drawing is safer on this MCU).

**Firmware structure:** simple state machine — idle/status screen showing sub-GHz hit count, menu mode navigated by the encoder/buttons, detail view for last captured signal.

**UART link to hub:** since the Uno has no wireless, it talks to the hub over a basic serial link — TX/RX crossed, common ground, line-based text protocol (see Integration Layer below).

**Standalone test milestones:**
- Confirm `rc-switch` decodes a known remote (garage door, cheap 433MHz doorbell) reliably.
- Confirm the big LCD renders and updates without visible tearing/flicker.
- Confirm buttons/encoder navigate a basic 2–3 screen menu.

---

## Module 4 — Hub Console (ESP32 + attached screen)

**Purpose:** central aggregator — receives data from both C3 nodes and the Arduino, renders a unified dashboard, and (optionally) logs everything to persistent storage.

**Core firmware pieces:**
1. **ESP-NOW receiver** — register a receive callback, parse incoming structs from each C3 node (see packet format below), keyed by sender MAC so you know which node sent what.
2. **UART receiver** — a second serial port (ESP32 has multiple UARTs) reading the Arduino's line-based reports.
3. **Aggregation store** — an in-memory table (RAM-permitting) or, better, log rolling summaries to an SD card if you add a module — raw per-node counts, unique-entity totals, and flagged-tracker alerts.
4. **Dashboard UI** — menu-driven on the attached screen: overview screen (counts from each node), WiFi detail list, BLE detail list + tracker alerts, sub-GHz detail list, node health (last-seen timestamp per node, so you know if one dropped offline).
5. **Optional:** put the hub into WiFi AP mode with a tiny local web dashboard — lets you view everything from a phone browser without touching the physical menu. This doesn't conflict with the sniffer node's promiscuous mode since they're separate radios/devices.

**Standalone test milestones (before any nodes are wired in):**
- Build the dashboard UI against fake/synthetic data first so the menu system, screens, and navigation are solid independent of the radio link.
- Confirm the screen library and pin mapping you're using (tell me the exact screen model when you get here and I'll give you the right driver + init code).

---

## Integration layer — how the standalone parts become one deck

### ESP-NOW protocol (C3 nodes → hub)
- Each node broadcasts (or unicasts to the hub's known MAC — unicast is more reliable and lets you enable ESP-NOW's built-in AES encryption via a PMK/LMK pair, worth doing since you're logging real MACs/SSIDs).
- Define one struct per report type, kept small (ESP-NOW payload cap is 250 bytes):
```c
typedef struct {
  uint8_t  node_id;      // 1 = wifi sniffer, 2 = ble hunter
  uint32_t ts;
  uint8_t  mac[6];
  char     label[20];    // SSID or BLE name, truncated
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  flags;        // e.g. bit0 = tracker-suspect
} deck_report_t;
```
- Hub's receive callback switches on `node_id` to route into the right in-memory table.

### UART protocol (Arduino → hub)
- Simple newline-delimited CSV, e.g.:
```
SUBGHZ,1691840213,rc-switch,4194305,24,3
```
`type,timestamp,protocol,code,pulse_len,repeats` — easy to parse on both ends, easy to debug by eye over a serial monitor.
- Baud rate 9600 or 115200 (Uno can do 115200 reliably over short wire runs) — pick one and keep it consistent both directions.

### Pairing/addressing
- Hardcode each C3 node's hub MAC address as the ESP-NOW peer at flash time — simplest, no dynamic discovery needed for a 3-node mesh.
- Print each device's own MAC on boot over serial so you can copy it into the hub's peer list.

### Power
- Each ESP32-C3 and the hub ESP32 can run off USB power banks or single-cell LiPo + a charge/boost board — budget ~80–120mA active WiFi/BLE scanning per node.
- Arduino Uno is the power hog of the bunch relatively speaking (linear regulator on most Uno boards) — fine on USB power, less ideal on battery if you want long runtime; a Nano/Pro Mini swap later would help but isn't required to get this working.

### Physical layout
- Think of it as a panel/tray: hub + its screen central, C3 nodes flanking it (with external antennas pointed outward if using u.FL boards), Arduino + big LCD as the bottom status bar, BLE hunter's small LCD detachable/handheld since that's the one module that benefits from leaving the tray to go find a tracker.

---

## Build order

1. **Phase 1 — standalone firmware.** Build and validate each of the four modules independently against serial/local-display output only. No inter-device communication yet.
2. **Phase 2 — ESP-NOW link.** Get one C3 node reporting to the hub, confirm packets arrive and parse correctly, then bring up the second C3 node.
3. **Phase 3 — UART link.** Wire the Arduino to the hub, validate the CSV protocol end-to-end.
4. **Phase 4 — unified dashboard.** Wire the hub's UI to real incoming data instead of the synthetic data used in Phase 1.
5. **Phase 5 — power + enclosure.** Battery selection, antenna mounting, physical tray/case assembly.

---

Use this as the outline to decompose per-module in the CLI — each module section above (sniffer, hunter, sub-GHz/controls, hub, integration) is sized to become its own detailed doc with exact wiring diagrams, full code, and a parts list once you confirm the exact board/screen models you're using for each.
