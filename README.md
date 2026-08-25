# War-rig-phantom (CyberDeck)

A portable OPSEC/OSINT recon deck: passive WiFi, BLE, and sub-GHz listening across three ESP32-C3 sensor nodes, aggregated on an ESP32 hub console with a display.

**Scope:** everything here is passive listening / logging on your own gear. No deauth transmission, no packet injection, no touching networks you don't own.

## Architecture

Three ESP32-C3 nodes each scan one RF domain and report over ESP-NOW broadcast to a central ESP32 hub, which owns all display, menu, and aggregation logic.

```
 ESP32-C3 #1          ESP32-C3 #2          ESP32-C3 #3
 WiFi sniffer         BLE hunter            Sub-GHz node
 (no display)      (0.96" I2C OLED)        (no display)
       \                  |                    /
        \                 | ESP-NOW            /
         \      broadcast deck_report_t       /
          \                |                 /
                    ESP32 Hub Console
                    (screen, aggregation,
                     dashboard, alerts)
```

This is an ESP-NOW-consolidated redesign (see `esp-now-consolidation` branch / `COMPONENTS.md`): an earlier design put sub-GHz sensing and a menu/control surface on an Arduino Uno + big LCD. That's been dropped — sub-GHz sensing now runs on a third ESP32-C3 node like the other two, and the hub owns all display/control. The old Uno sketch remains in git history / on `main` if needed.

## Components

| # | Component | Device | Status |
|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | 🟨 compiles clean, not yet flashed/tested |
| 2 | [BLE Hunter Node](02-ble-hunter/README.md) | ESP32-C3 #2 + 0.96" I2C OLED | 🟨 in progress — core scan/log confirmed live, persistence/tracker-flag logic flashed, live re-confirmation pending |
| 3 | [Sub-GHz Node](03-subghz-control/README.md) | ESP32-C3 #3, no display | 🟨 compiles clean, not yet flashed/tested |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + screen | 🟨 compiles clean, not yet flashed/tested |
| 5 | [Integration Layer](05-integration/README.md) | all of the above | ⬜ not started — depends on 1–4 passing standalone |

Full details and build order live in [COMPONENTS.md](COMPONENTS.md). Each numbered folder is a standalone, individually-testable component with its own build checklist.

## Docs

- [`docs/opsec-osint-deck-plan.md`](docs/opsec-osint-deck-plan.md) — original implementation plan
- [`docs/deck-component-availability.md`](docs/deck-component-availability.md) — parts sourcing
- [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) — remote flashing station setup

## License

See [LICENSE](LICENSE).
