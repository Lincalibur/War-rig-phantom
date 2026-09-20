# CyberDeck v2 — PCB design (KiCad input)

Fresh-start hardware design for a portable wardriving deck. Supersedes the ESP-NOW multi-node design in `PINOUTS.md` (kept for the old build). Board size: **100 x 150 mm**, 2-layer.

**The authoritative BOM and pin-by-pin netlist is `PCB-BOM-AND-NETLIST.md`. If the two differ, that file wins** (it adds the back-feed diodes, VBAT sense, solder jumpers, exact quantities and corrected node pin labels).

**Status: pin assignments are a first draft.** Items marked VERIFY must be checked against the actual module/devkit datasheets before the schematic is frozen.

## Architecture

```
                 +----------------------------+
   GPS (UART) -->|  fan-out to both nodes     |
                 +----------------------------+
                    |                    |
              +-----v-----+        +-----v-----+
              | XIAO C5   |        | XIAO C6   |
              | WiFi 2.4+5|        | BLE+802.15|
              +-----+-----+        +-----+-----+
                    | UART1              | UART2   (921600 baud, 3.3V, wired)
                    +---------+  +-------+
                          +---v--v---+        SPI bus --- microSD
                          | ESP32-S3 |------- (shared) -- CC1101 (sub-GHz)
                          |   HUB    |------- SPI ------- 2.8" TFT
                          +----------+------- GPIO ------ 5 nav buttons
```

Key idea: **nodes never transmit**. They are receive-only scanners and report over wired UART, so the ESP-NOW timing and near-field desense problems from the old design go away. Each node reads the GPS directly and tags its own sightings with a location.

## Bill of materials

| Ref | Part | Notes |
|---|---|---|
| U1 | ESP32-S3-DevKitC-1 (**N8R2** or N16R2) | Avoid R8/octal-PSRAM variants: they consume GPIO33-37 |
| U2 | Seeed XIAO ESP32-C5 | 2.4 + 5 GHz WiFi sniffer. VERIFY: Arduino promiscuous-mode support on C5 |
| U3 | Seeed XIAO ESP32-C6 | BLE + 802.15.4. Has U.FL antenna connector |
| U4 | CC1101 module | Buy the band matching your region (433 vs 868/915 MHz) — matching network is band-specific |
| U5 | u-blox M10 GPS module (BN-220 class) | UART, 3.3V, ceramic patch antenna |
| DISP1 | 2.8" SPI TFT (ILI9341, 320x240) | ~50x70 mm, no touch needed |
| J_SD | microSD breakout or SMD slot | SPI mode |
| PWR1 | Adafruit PowerBoost 1000C (or equivalent 5V boost+charger with load-sharing) | Handles charge, boost and load-share. Do not build this on the PCB. See Power |
| BT1 | 1S LiPo, 2000-3000 mAh, protected | JST-PH |
| SW1-5 | 5 tactile buttons (up/down/left/right/select) | Or one 5-way nav switch |
| SW_PWR | Slide switch on PowerBoost EN line | |
| F1 | 1.5A PTC polyfuse | On 5V rail after the module |
| C_bulk | 470 uF electrolytic + 100 uF ceramic/tantalum per socket | See Power |
| Sockets | 2x 1x7 female headers per XIAO, 2x22 for DevKitC, etc. | Socketed so boards can be swapped and reflashed |

## Pin map

### Hub (ESP32-S3-DevKitC-1) — VERIFY against the devkit pinout

Avoid GPIO0, 3, 45, 46 (strapping), 19/20 (USB), 26-32 (flash/PSRAM).

| Function | GPIO | Connects to |
|---|---|---|
| SPI SCLK (shared) | 12 | TFT, SD, CC1101 |
| SPI MOSI (shared) | 11 | TFT, SD, CC1101 |
| SPI MISO (shared) | 13 | SD, CC1101 (TFT MISO unused) |
| TFT CS | 10 | DISP1 |
| TFT DC | 9 | DISP1 |
| TFT RST | 8 | DISP1 |
| TFT backlight (PWM) | 7 | DISP1 via transistor if current > 20 mA |
| SD CS | 14 | J_SD |
| CC1101 CS | 15 | U4 |
| CC1101 GDO0 | 16 | U4 |
| CC1101 GDO2 | 17 | U4 |
| UART1 RX / TX | 4 / 5 | C5 TX / RX (crossed) |
| UART2 RX / TX | 1 / 2 | C6 TX / RX (crossed) |
| Button up/down/left/right/select | 38 / 39 / 40 / 41 / 42 | to GND, internal pull-ups |
| Battery-low (LBO) | 47 | PowerBoost LBO |
| Spare | 18, 21, 48 | keep on a header |

### Nodes (XIAO C5, XIAO C6)

| Function | Node pin | Connects to |
|---|---|---|
| Hub link TX/RX | D6 / D7 (default UART) — VERIFY GPIOs per board | Hub RX / TX (crossed) |
| GPS RX | D2 (remap a second UART) | GPS TX (one GPS TX fans out to both nodes) |
| Power | 5V pin | 5V rail |
| Ground | GND | GND |

### GPS
| GPS pin | Connects to |
|---|---|
| VCC | 3.3V from hub 3V3 (check module is 3.3V-safe) |
| GND | GND |
| TX | C5 GPS RX and C6 GPS RX |
| RX | not connected (module runs on defaults; configure 5-10 Hz beforehand via USB-TTL) |

## Power

Don't design the charge/boost stage. Use a finished module and only add filtering on the 5V rail it feeds.

1. **Module:** PowerBoost 1000C (or similar). Provides LiPo charging, 5V boost, load-sharing (runs while charging) and a low-battery flag. It's rated about 1A. Estimated peak draw is ~900 mA (nodes ~250 mA each peak, hub + TFT ~300 mA, GPS + CC1101 ~80 mA), so this is tight. If you see brownouts, move to a 2A-class module. Budget accordingly.
2. **Protection:** 1.5A PTC polyfuse in series on 5V, then a Schottky diode if any external 5V input is ever added.
3. **Bulk capacitance:** 470 uF electrolytic at the 5V entry, plus 100 uF close to each node socket and the hub. WiFi/BLE bursts draw 300+ mA in microseconds and starved supplies cause the random resets you're worried about.
4. **Grounds:** one solid ground plane under everything, no splits. Star the module's return at the plane.
5. **Route power wide:** at least 1 mm traces on the 5V rail, and 1.5 mm+ from the module to the bulk cap.
6. **Low battery:** wire LBO to hub GPIO47 so the screen can warn and the logger can flush and close the SD file before power dies.

## Placement on 100 x 150 mm

Long axis vertical. Front face = screen.

```
        <------- 100 mm ------->
  +---------------------------------+  ^
  | [GPS patch, clear sky]          |  |
  |                                  |  |
  |   +---------------------------+ |  |
  |   |   2.8" TFT + hub (top     | |  |
  |   |   face, screen outward)   | | 150
  |   +---------------------------+ | mm
  |  [nav buttons]                  |  |
  |                                  |  |
  | [C5]  (bottom face)  [C6]       |  |
  | [SD]  [CC1101]    [PowerBoost]  |  v
  +---------------------------------+
```

- **Top face:** hub and screen. **Bottom face:** C5, C6, CC1101, SD, PowerBoost, LiPo.
- **C5 and C6 at opposite short edges (~10 cm apart).** With wired links neither transmits, so this is precautionary, not the hard requirement it was in the ESP-NOW design.
- **Ground plane between the hub side and the node side** shields them from the backlight/SPI noise.
- **GPS at the top edge:** no copper pour or components over/near the patch antenna.
- **XIAO antenna keep-out:** no copper pour or traces under the antenna end of each XIAO. Put the U.FL on the C6 to an external antenna if the case blocks the trace antenna. VERIFY C5 antenna option.
- **CC1101 antenna:** at the board edge, away from the C5/C6 antennas, with its keep-out respected.
- **USB-C ports:** leave the USB-C of every socketed board reachable from the case edge so each can be reflashed without unplugging it.
- **Depth budget:** ~25 mm total (hub + display on top, nodes/LiPo below).

## Firmware implications (not in this doc's scope, noted for later)
- Node firmware: strip ESP-NOW, keep promiscuous/BLE scan, add GPS parse, send framed binary records over UART.
- Hub firmware: rewrite for S3 + ILI9341 (TFT_eSPI or LovyanGFX), SD logging in WiGLE CSV, UART frame parser per node.
- Existing `deck_report.h` frame formats can inform the UART protocol but the transport and framing change.

## VERIFY before ordering PCBs
1. XIAO ESP32-C5 promiscuous-mode maturity in Arduino/ESP-IDF; fall back to a second C6 if not ready.
2. Exact GPIOs for D2/D6/D7 on each XIAO.
3. S3 devkit variant (avoid octal PSRAM) and that GPIO 1, 2, 4-18, 21, 38-42, 47, 48 are free on your specific devkit.
4. PowerBoost current rating vs. measured peak draw on the bench before committing to 1A.
5. CC1101 band for your region.
