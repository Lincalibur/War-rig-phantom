# SENTINEL — passive counter-surveillance watcher (plan + build notes)

**Status: IN BUILD (started 2026-11-13).** Firmware: `sentinel/v1/c3` + `sentinel/v1/uno`.

## What SENTINEL is

**SENTINEL** is the soldered **ESP32-C3 + Arduino UNO (3.5" mcufriend TFT)** pair, repurposed from
the old WIFI-NODE/READOUT + orchestrator role. With the active phone-driven toolkit moving onto
**SPECTRE** (the ESP32-DIV handheld — see `docs/hub-migration-plan.md`), SENTINEL takes the opposite
job: a **standalone, always-on, passive watcher** that never transmits. No phone, no BLE control
link, no attacks — it just listens and warns you on its own screen + LED.

It answers three counter-surveillance questions at once:

1. **Trackers** — "is an AirTag / Tile / offline-finding tag following me?"
2. **Tails** — "is a Wi-Fi device reappearing around me over time (a possible tail)?"
3. **Attacks** — "is someone deauth-flooding or standing up an evil-twin AP near me?"

This is the **defensive mirror** of SPECTRE: SPECTRE can run those attacks (own-lab); SENTINEL detects
them. The two never duplicate each other.

## Why all three fit on one single-radio C3

The ESP32-C3 has **one 2.4 GHz radio**, so Wi-Fi promiscuous and BLE scanning can't both receive at
the same instant. Two things make the combo practical anyway:

- **Trackers vs. (Tails + Attacks) is the only real split.** Tails and Attacks are *both* derived
  from the **same Wi-Fi promiscuous capture** — deauth/disassoc frames and evil-twin beacons are just
  802.11 management frames we're already sniffing while building the device table. One pass, two
  detectors, no extra radio time.
- **So SENTINEL just time-slices Wi-Fi vs BLE** in a round-robin:

  ```
  ┌─ WIFI window (~8 s) ─ promiscuous, channel-hop 1..13 ─┐   feeds Tails + Attacks
  └─ BLE  window (~4 s) ─ passive scan ───────────────────┘   feeds Trackers
                      … repeat forever …
  ```

Both stacks stay **initialized for the whole run**; each slice only toggles `promiscuous on/off` and
`scan start/stop`. We do **not** init/deinit NimBLE every cycle — that's the fragile, heap-fragmenting
path, and this device runs for hours. Only one RX path is ever active at a time, so there's no
coexistence gamble on the hot path. *(Fallback if the bench shows trouble: full wifiStop()↔bleStop()
handoff like the orchestrator. Structured so that's a small change.)*

## Division of labor

- **SENTINEL-C3** (brain): the round-robin sniffer + all detection/persistence logic. Pushes finished
  rows to the UNO over the one-way UART link. Drives the two LEDs (via the UNO).
- **SENTINEL-TFT** (Arduino UNO + 3.5"): dumb terminal, unchanged protocol from the orchestrator —
  `R<rr><c><text>` draw row, `C` clear, `L<g><r>` LEDs. Green LED = healthy/scanning, **Red LED =
  active alert**. The screen is the source of truth; the LED can go stale if the link drops.

Link: C3 GPIO21 (Serial1 TX) → UNO D0 (RX), common GND, **19200 baud**. Identical wiring to the
orchestrator, so no hardware change — just reflash both boards.

## Detection logic

### Persistence is the heart of all three
Every device (Wi-Fi AP, Wi-Fi client, BLE device) carries `firstMs`, `lastMs`, and a **`sweeps`
counter** (how many distinct scan windows it has appeared in). "Following me" = high `sweeps` over a
long `lastMs-firstMs`, not just a strong RSSI right now. State lives in **RAM only** (no RTC, no SD),
so it resets on power-cycle — fine for an always-on session; surviving reboots would need added
storage (deferred; see open questions).

### Trackers (BLE window)
- Reuse `classifyBle()`: AirTag/offline-finding (Apple mfr `0x4C00`, type `0x12`), Flipper (name),
  skimmer-class serial-BT modules (HC-0x / AT-09 / MLT-BT05 / JDY).
- A tracker-class device with `sweeps ≥ TRACKER_FOLLOW_SWEEPS` (default 3) **and** a lifespan over
  `FOLLOW_MIN_MS` → **ALERT** (red LED). Rank by sweeps then lifespan.

### Tails (Wi-Fi window)
- Reuse the client table (`cliUpsert` on data frames + probe requests).
- A **non-randomized** client MAC (locally-administered bit clear) with high `sweeps` + long lifespan
  → **tail ALERT**. Randomized MACs (modern phones) are shown but down-weighted — they can't be
  tracked across sessions, so they're informational, not alerts.

### Attacks (Wi-Fi window, same capture)
- **Deauth/disassoc flood:** rate of subtype-0xC/0xA frames over a sliding window. Above
  `DEAUTH_ALERT_RATE` (frames/10 s) → **ALERT**.
- **Evil-twin / SSID collision:** two APs with the **same SSID but different BSSID** → flag the SSID.
- **Beacon flood:** abnormally high rate of *new* BSSIDs appearing per second → flag.

### Alert/LED rule
Red LED + top banner on **any** active alert (tracker-follow, tail, deauth flood, evil-twin). Green
LED when scanning clean. The banner names the worst current alert.

## UNO screen layout (rows 0-18, 40 cols, size-2 text)

```
0   SENTINEL  <phase> ch.. up..s                 (cyan header)
1   <ALERT banner | "all clear">                 (red if alerting, else green)
2   TRACKERS  kind  rssi sweeps age               (yellow)
3-6   up to 4 tracker rows                        (red if following, else gray/green)
7   TAILS  rssi sweeps age  mac  ->/probe         (yellow)
8-12  up to 5 tail rows                           (orange if sticky, gray if random/stale)
13  ATTACKS  deauth/10s  eviltwins  newAP/s       (yellow)
14-17 attack detail rows (deauth rate bar, evil-twin SSIDs)
18  stats: frames, APs, CL, BLE seen             (green)
```

## Firmware notes carried from the orchestrator
- **Auto-prototype gotcha** (`[[arduino_ide_prototype_struct_ordering]]`): any enum/struct used as a
  function param/return must be defined above the first function. Keep `BleKind` etc. up top.
- **UNO AVR printf has no `*` width** — use literal `%-40.40s` (`[[avr_printf_no_star_width]]`).
- **`setRow()` paces ~150 ms** per row so the UNO's 64-byte RX buffer never overflows; the C3 only
  streams a bounded set of rows per refresh.
- Build/flash: `arduino-cli compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" sentinel/v1/c3`.
  Power the C3 down before flashing the UNO (its D0 bytes corrupt the UNO bootloader).

## Build phases
0. **Skeleton + round-robin** — Wi-Fi/BLE time-slice, header + liveness on the UNO. *(this commit)*
1. **Tails + Attacks** off the Wi-Fi window — client persistence, deauth rate, evil-twin.
2. **Trackers** off the BLE window — classify + persistence + follow alert.
3. **Alert/LED unification** — banner + red LED on any alert.
4. **Tuning on the bench** — thresholds (`TRACKER_FOLLOW_SWEEPS`, `DEAUTH_ALERT_RATE`, windows).

## Open questions / deferred
- **Reboot-surviving history** — needs NVS or SD; deferred (RAM-only for now).
- **MAC-randomization defeats long-term Wi-Fi tailing** — inherent; BLE tracker detection and
  in-session Wi-Fi persistence still work. Could later correlate by probe-SSID fingerprints.
- **Window lengths** (8 s / 4 s) are a starting guess — tune for how fast a tail would move vs. BLE
  advertising intervals.
