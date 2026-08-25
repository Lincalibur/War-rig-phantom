# Component 1 — WiFi Sniffer Node

**Device:** ESP32-C3 #1
**Toolchain:** ESP-IDF (not Arduino core — need raw promiscuous-mode callbacks)
**Status:** 🟨 `firmware/wifi_sniffer/wifi_sniffer.ino` (steps 1.1-1.6) compiles clean against `esp32:esp32:esp32c3:CDCOnBoot=cdc`, not yet flashed/tested on hardware

Passively captures 802.11 management frames (beacons, probe req/resp) to map nearby APs and catch devices leaking their preferred-network list. No transmission, no injection.

## Build in this order (each sub-task independently testable)

- [ ] **1.1 Promiscuous mode init** — `WIFI_MODE_NULL`, register RX callback, `esp_wifi_set_promiscuous(true)`.
  - Test: serial log fires on any nearby WiFi traffic.
- [ ] **1.2 Channel hopper** — cycle channels 1–13 on a timer (~300ms), log channel per hop.
  - Test: log confirms all 13 channels get visited over a run.
- [ ] **1.3 Frame filter** — keep `WIFI_PKT_MGMT`, drop data/control frames.
  - Test: no data-frame noise in log output.
- [ ] **1.4 802.11 header parsing** — subtype (beacon 0x80, probe req 0x40, probe resp 0x50), src/dst/BSSID MAC, walk tagged params for SSID (tag 0).
  - Test: serial-print every beacon seen nearby, confirm SSID/RSSI look correct.
- [ ] **1.5 Dedup/aggregation table** — hash table keyed on BSSID+SSID, track first-seen/last-seen/RSSI min-max/hit count. Cap size, evict oldest on overflow (C3 has ~400KB usable RAM).
  - Test: run 10 min in a busy area, confirm memory stays bounded.
- [ ] **1.6 Deauth/disassoc counter** — flag reason-code frames (subtype 0x0C/0x0A), count only, no action.
  - Test: trigger/observe a deauth spike (e.g. near a captive-portal AP) and confirm counter moves.
- [ ] **1.7 Probe-request validation pass** — separately confirm probe-req capture works (privacy-relevant: phones broadcasting known SSIDs while idle).

## Output record (per unique entity)
```
{ ts, mac, ssid, rssi, channel, frame_type, hits }
```

## Standalone-complete checklist
- [ ] Status LED (scanning / hop-active) working
- [ ] All above sub-tasks pass their individual test
- [ ] Runs unattended 10+ min without crash or memory growth
- No display needed on this node — all output goes to the hub once integrated (see `../05-integration`).
