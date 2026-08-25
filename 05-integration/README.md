# Component 5 — Integration Layer

**Status:** ⬜ not started (depends on Components 1–4 passing standalone)

How the four standalone parts become one deck. Nothing here starts until each node has passed its own standalone-complete checklist.

## 5.1 ESP-NOW protocol (C3 nodes → hub)
- [ ] Unicast to hub's known MAC (not broadcast) — enables ESP-NOW AES via PMK/LMK, worth it since you're logging real MACs/SSIDs.
- [ ] Shared struct header (put in `shared/deck_report.h`, included by both node and hub firmware):
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
  (ESP-NOW payload cap is 250 bytes — keep it small.)
- [ ] Hub's RX callback switches on `node_id`, routes into the right table.
- **Bring-up order:** wire ONE C3 node to the hub first, confirm packets arrive and parse, THEN bring up the second C3 node.

## 5.2 UART protocol (Arduino → hub)
- [ ] Newline-delimited CSV: `type,timestamp,protocol,code,pulse_len,repeats`
  - e.g. `SUBGHZ,1691840213,rc-switch,4194305,24,3`
- [ ] Baud rate: pick 115200 (Uno handles it fine over short wire runs), same both directions.
- [ ] Physical: TX/RX crossed, common ground.

## 5.3 Pairing/addressing
- [ ] Hardcode each C3 node's hub-MAC as its ESP-NOW peer at flash time (no dynamic discovery needed for a 3-node mesh).
- [ ] Each device prints its own MAC on boot over serial — copy into hub's peer list.

## 5.4 Power
- [ ] Each ESP32-C3 + hub ESP32: USB power bank or single-cell LiPo + charge/boost board. Budget ~80–120mA active per node.
- [ ] Arduino Uno: fine on USB, less ideal on battery (linear regulator = power hog relatively). Nano/Pro Mini swap is an optional later upgrade, not required for v1.

## 5.5 Physical layout
- [ ] Panel/tray: hub + screen central, C3 nodes flanking (external antennas outward if u.FL boards), Arduino + big LCD as bottom status bar, BLE hunter's small LCD detachable/handheld.

## Integration build order
1. Phase 2 — one C3 node → hub over ESP-NOW, validate, then second C3 node.
2. Phase 3 — Arduino → hub over UART, validate CSV end-to-end.
3. Phase 4 — swap hub's synthetic dashboard data (Component 4.1) for real incoming data.
4. Phase 5 — power + enclosure, physical tray assembly.
