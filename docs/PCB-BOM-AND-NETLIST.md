# CyberDeck v2 — exact BOM and netlist (KiCad input)

Authoritative parts list and pin-by-pin connections for the 100 x 150 mm board. Where any other doc differs from this one, **this file wins** (see `OVERVIEW.md` for the why). As of the 2026-09-25 nde3d revision, the node boards changed (C3 PRO Mini / H2 SuperMini), so the firmware in `../v2/` still has XIAO pin labels and needs updating to match.

Items marked **[VERIFY]** are footprint/pinout facts recalled from memory of the manufacturer's documentation. Check each against the datasheet or the physical board before sending the PCB to fabrication.

## 1. Components

**Revision 2026-09-25: sourced from nde3d.co.za (Next Dimension Electronics).** nde3d doesn't stock the XIAO C5/C6, the PowerBoost 1000C, LiPo pouch cells, 6x6 tactile switches or PTC fuses, so those were substituted. Section 1.4 says what changed and why. Prices are as nde3d listed them on 2026-09-25.

### 1.1 Modules (all plug into socket headers; nothing is soldered down)

| Ref | Part | Qty | Board-side footprint | Notes |
|---|---|---|---|---|
| U1 | ESP32-S3-DevKitC-1 **N16R8**, [nde3d](https://nde3d.co.za/esp-devices/3511-esp32-s3-n16r8-devkitc-development-board-with-wireless-53-16mb-flash-8mb-ram-type-c-usb.html) R188.86 | 1 | 2x 1x22 female 2.54 mm, rows ~22.86 mm apart **[VERIFY]** | Hub. The octal PSRAM uses GPIO33-37, which this netlist already leaves free. In Arduino, set PSRAM = "OPI PSRAM" |
| U2 | ESP32-C3 PRO Mini (ceramic antenna + IPEX), [nde3d](https://nde3d.co.za/esp-devices/3722-esp32-c3-supermini-wifi-bluetooth-compatible-board-supermini-development-board-development-board-core-board-for-arduino.html) R110 | 1 | 2x 1x8 female 2.54 mm, SuperMini-style **[VERIFY pinout + row spacing]** | WiFi node, **2.4 GHz only**. Replaces the XIAO C5 |
| U3 | ESP32-H2 SuperMini, [nde3d](https://nde3d.co.za/esp-devices/3991-esp32-h2-supermini-development-board.html) R101.82 | 1 | 2x 1x8 female 2.54 mm **[VERIFY pinout + row spacing]** | BLE node (802.15.4 later). Has no WiFi. Replaces the XIAO C6. If H2 BLE gives trouble in Arduino, fall back to the ESP32-C3 SuperMini ([nde3d](https://nde3d.co.za/esp-devices/3413-esp32-c3-supermini-wifi-bluetooth-compatible-board-supermini-development-board-development-board-core-board-for-arduino.html) R91) |
| DISP1 | 2.8" SPI TFT, ILI9341, 320x240, 14-pin header, [nde3d](https://nde3d.co.za/displays/2409-28-inch-tft-lcd-colorful-screen-display-module.html) R468 (**only 1 in stock**) | 1 | 1x14 female 2.54 mm | Leave the touch pins unconnected. It has its own SD slot, which is unused (J_SD is used instead) |
| U4 | CC1101 433 MHz with spring antenna + IPEX, [nde3d](https://nde3d.co.za/rf-modules/2959-433mhz-cc1101-wireless-rf-transceiver-module.html) R114 | 1 | 1x8 2.54 mm **[VERIFY pin order; it varies by vendor]** | 3.3 V only. 433 MHz band chosen for SA |
| U5 | GY-NEO-7M GPS with ceramic patch antenna, [nde3d](https://nde3d.co.za/rf-modules/3191-gy-neo-7m-gps-active-ceramic-antenna-module.html) R136.48 | 1 | 1x4 2.54 mm **[VERIFY pin order]** | Pins: VCC, GND, TX, RX. Replaces the NEO-M8N/M10, which nde3d doesn't stock |
| J_SD | Micro TF/SD shield for D1 Mini (plain 3.3 V SPI), [nde3d](https://nde3d.co.za/modules/779-micro-tfsd-card-shield-for-d1-mini.html) R24 | 1 | wired to 1x6 2.54 mm **[VERIFY: uses the D1 Mini header's 3V3, GND and D5-D8 pins]** | Do NOT use nde3d's "SPI SD Card Module": it has a full-size slot and 5 V circuitry |
| PB1 | IP5310 charge + 5 V boost power-bank board, USB-C, 3.1 A out, [nde3d](https://nde3d.co.za/power-supplies/3267-5v-to-24v-3a-lithium-battery-charging-boosting-module.html) R62 (**only 4 in stock**) | 1 | wired to a 2-pin 5V/GND pad pair and a 2-pin BAT pad pair **[VERIFY pads]** | Replaces the PowerBoost 1000C. It has no EN or LBO pin. Protection against over-charge, over-discharge and shorts is built in. Backup: 5V 2A module ([nde3d](https://nde3d.co.za/power-supplies/3272-5v-2a-charge-and-discharge-boost-module-37v42v.html) R27) |
| BT1 | Sanyo 18650 3300 mAh Li-ion, [nde3d](https://nde3d.co.za/consumables/2201-18650-li-ion-battery-3000mah.html) R189.40 (forces road freight), plus a 1-cell holder with wires, [nde3d](https://nde3d.co.za/battery-holders/2436-18650-battery-holder-with-wires.html) R7 | 1 | holder on the bottom face, wired to PB1 BAT | Replaces the 1S LiPo pouch (none in stock). Alternative: 2200 mAh 18650 at R79.40, but its listing also says 1.5 Ah |
| SW_NAV | Five-way navigation button module (up/down/left/right/mid, SET/RST, COM), [nde3d](https://nde3d.co.za/buttons/1744-five-way-navigation-button-module.html) R21.21 | 1 | 1x7 female 2.54 mm **[VERIFY pin order]** | Replaces SW1-SW5. SET and RST are unused. COM goes to GND |

### 1.2 Board-level parts

| Ref | Part | Qty | Footprint | Source / notes |
|---|---|---|---|---|
| SW_PWR | Mini slide switch | 1 | THT **[VERIFY pitch]** | [nde3d](https://nde3d.co.za/switches/1738-mini-slide-switch-with-mounting-hole-pack-of-3.html) R10 for 3. In series with the PB1 5 V output, because PB1 has no EN pin |
| F1 | **DNP: bridge with wire or a 0 Ohm link.** Keep the radial 5 mm footprint | (1) | radial 5 mm THT | nde3d has no resettable fuse, and PB1 has output short protection. Fit a 1.1 A PTC later if you source one elsewhere |
| D1-D3 | 1N5819W Schottky | 3 | **SOD-323** (was DO-41) | [nde3d](https://nde3d.co.za/diodes/3391-1n5819w-smd-schottky-diode-sod123323523.html) R5 for 10. One per module 5 V input, to block USB back-feed |
| C1 | 470 uF 35 V electrolytic | 1 | radial 8 mm THT **[VERIFY diameter]** | [nde3d](https://nde3d.co.za/capacitors/3347-22uf-16v-electrolytic-capacitor-pack-of-5.html) R20 for 5 |
| C2-C4 | **220 uF 16 V** electrolytic (100 uF is out of stock) | 3 | radial 6.3 mm THT | [nde3d](https://nde3d.co.za/capacitors/1817) R6 for 5 |
| C5-C11, C13 | 100 nF 50 V ceramic | 8 | **THT, 5 mm pitch** (was 0805) | [nde3d](https://nde3d.co.za/capacitors/2832-100nf-50v-10-ceramic-cap-pack-of-5.html) R5.07 for 5; buy 2 packs |
| C12 | 10 uF 50 V ceramic | 1 | THT, 5 mm pitch | [nde3d](https://nde3d.co.za/capacitors/3634-100nf-50v-10-ceramic-cap-pack-of-5.html) R5.07 for 5. On GPS VCC |
| R1, R2 | 100 kOhm 1% 1/4 W metal film | 2 | **THT axial** (was 0805) | [nde3d](https://nde3d.co.za/resistors/503-resistor-thl-14w-1-pack-of-10.html) R5 for 10. The VBAT divider is now the **only** low-battery signal |
| R3-R7 | 10 kOhm | — | removed | The buttons moved to SW_NAV, which uses the internal pull-ups |
| R8 | 1 kOhm 1/4 W | 1 | THT axial | Same listing as R1 |
| LED1 | 3 mm green LED | 1 | 3 mm THT | [nde3d](https://nde3d.co.za/leds-bulbs/469-3mm-green-led-diffused-pack-of-5.html) R12 for 5 |
| JP1 | 3-pad solder jumper | 1 | | Backlight: pad A = always on (3V3), pad B = GPIO7 control. **Default: bridge A** |
| J_SPARE | 1x6 pin header | 1 | 2.54 mm | 3V3, GND, GPIO18, GPIO21, GPIO48, 5V |
| TP1-TP3 | Test points | 3 | 1 mm pad | 5V, 3V3, GND |
| H1-H4 | M3 mounting holes | 4 | 3.2 mm | 4 mm in from each corner. [M3 nylon standoff kit](https://nde3d.co.za/bolts-nuts/3354-sdafasdf.html) R98.78 |

### 1.3 Headers, extras, cost
- 2.54 mm female header: the [40-way strip](https://nde3d.co.za/plugs-sockets-connectors/1503-40-way-female-pin-header-single-row-254mm-01.html) is R7.50; buy **4**. The board needs about 116 pins: 1x22 x2, 1x8 x4, 1x14, 1x8, 1x7, 1x6 and 1x4.
- nde3d doesn't sell a suitable 2.4 GHz IPEX/U.FL antenna for U2 (the stick-on one is out of stock). It's optional; the onboard ceramic antenna works.
- You also need an enclosure and USB-C cables (one per module for flashing).
- **The core nde3d cart is about R1,630**, plus R118 for spares (C3 SuperMini and the 5V 2A module). Shipping is free over R2,000. The Sanyo 18650 forces road freight.

### 1.4 What changed from the original BOM, and the consequences
| Change | Consequence |
|---|---|
| XIAO C5 -> ESP32-C3 PRO Mini | **No 5 GHz capture**, but most dual-band APs also beacon on 2.4 GHz. The node footprint changes, and the firmware board target changes from `XIAO_ESP32C5` to generic ESP32C3. The UART and GPS pins need remapping **[VERIFY]** |
| XIAO C6 -> ESP32-H2 SuperMini | Keeps BLE and 802.15.4. This node doesn't need WiFi. First confirm that Arduino-ESP32 3.x BLE scanning compiles and runs on the H2; if not, fall back to the C3 SuperMini |
| PowerBoost 1000C -> IP5310 board | The 3.1 A output removes the ~1 A budget risk. It has **no EN pin**, so SW_PWR moves to the 5 V output. It has **no LBO pin**, so low battery comes from VBAT_SENSE (GPIO6), which the firmware must now read. Charge-while-running (pass-through) isn't documented, so bench-test plugging in USB-C while the deck runs. Expect a possible brief dropout |
| 1S LiPo -> 18650 + holder | The cell is 18 mm tall on the bottom face, which fits the ~25 mm depth budget. The IP5310 provides the cell protection |
| SW1-SW5 -> 5-way nav module | One 1x7 header replaces five switches |
| PTC fuse -> DNP | Relies on PB1's short-circuit protection |
| 0805 passives -> THT | Easier to hand-solder. The footprints change |
| NEO-M8N -> NEO-7M | Lower maximum update rate, which is fine for wardriving |

## 2. Power tree

```
BT1 (18650 in holder) --wires--> PB1.BAT (IP5310 board)
PB1 5V --> SW_PWR --> F1 (DNP, bridged) --> C1 470uF --> 5V_RAIL
5V_RAIL --> D1 --> C2 --> U2.5V   (ESP32-C3 PRO Mini, WiFi)
5V_RAIL --> D2 --> C3 --> U3.5V   (ESP32-H2 SuperMini, BLE)
5V_RAIL --> D3 --> C4 --> U1.5V   (S3 devkit)
5V_RAIL --> R8 --> LED1 --> GND
U1.3V3 --> 3V3_HUB  (powers DISP1, U4, U5, J_SD, J_SPARE)
GND common everywhere (one solid ground plane)
PB1.BAT --> R1 --> VBAT_SENSE --> R2 --> GND ; C13 across R2 ; VBAT_SENSE --> U1.GPIO6
```

3.3 V load on U1's onboard regulator (~800 mA LDO): display ~100 mA + GPS ~50 mA + CC1101 ~35 mA + SD burst ~100 mA + S3 itself ~150-350 mA. Peak is close to the LDO limit. If the S3 browns out under load, power the display/SD from a separate 3.3 V regulator — don't reduce anything else first.

5 V budget: WiFi node ~250-300 mA peak, BLE node ~150 mA, S3 ~300-350 mA including 3V3 loads, display ~100 mA. **~900 mA-1 A peak.** The IP5310 board is rated 3.1 A, so there is headroom; still measure it on the bench. The C3/H2 nodes draw somewhat less than the C5/C6 estimates.

## 3. Netlist

### 3.1 Hub U1 (ESP32-S3-DevKitC-1) — GPIOs assigned

Pins avoided on purpose: GPIO0, 3, 45, 46 (strapping), 19/20 (USB), 26-32 (flash/PSRAM), 33-37 (kept free of octal PSRAM).

| Net | U1 GPIO | Connects to |
|---|---|---|
| SPI_SCLK | 12 | DISP1.SCK, J_SD.SCK, U4.SCK |
| SPI_MOSI | 11 | DISP1.SDI(MOSI), J_SD.MOSI, U4.MOSI(SI) |
| SPI_MISO | 13 | J_SD.MISO, U4.MISO(SO) — **DISP1.SDO is NOT connected** (avoids contention) |
| TFT_CS | 10 | DISP1.CS |
| TFT_DC | 9 | DISP1.DC/RS |
| TFT_RST | 8 | DISP1.RESET |
| TFT_BL | 7 | JP1 pad B (backlight control) |
| SD_CS | 14 | J_SD.CS |
| CC_CS | 15 | U4.CSN |
| CC_GDO0 | 16 | U4.GDO0 |
| CC_GDO2 | 17 | U4.GDO2 |
| WIFI_LINK_RX | 4 | U2 NODE_TX |
| WIFI_LINK_TX | 5 | U2 NODE_RX |
| BLE_LINK_RX | 1 | U3 NODE_TX |
| BLE_LINK_TX | 2 | U3 NODE_RX |
| BTN_UP | 38 | SW_NAV.UP |
| BTN_DOWN | 39 | SW_NAV.DOWN |
| BTN_LEFT | 40 | SW_NAV.LEFT |
| BTN_RIGHT | 41 | SW_NAV.RIGHT |
| BTN_SEL | 42 | SW_NAV.MID |
| VBAT_SENSE | 6 | midpoint of R1/R2. **The only low-battery signal now; firmware must implement it** |
| SPARE | 18, 21, 48 | J_SPARE |
| (unused) | 47 | was PB1.LBO; the IP5310 board has no LBO pin |
| 5V | 5V pin | D3 cathode (via C4) |
| 3V3 | 3V3 pin (either) | 3V3_HUB net |
| GND | GND pins | GND |

Header positions on the DevKitC-1 (left row J1: 3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5V, GND; right row J3: GND, TX, RX, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, GND, GND) **[VERIFY against the Espressif DevKitC-1 schematic before drawing the footprint]**.

### 3.2 Nodes U2 (ESP32-C3 PRO Mini) and U3 (ESP32-H2 SuperMini): identical logical wiring

The pin labels are logical. **Map each one to a real GPIO from the board's pinout before drawing the footprint [VERIFY].** Avoid the strapping pins (C3: GPIO2, 8, 9; H2: GPIO8, 9, 25). On the C3, GPIO20/21 are the default UART0 RX/TX. The v2 firmware still uses the XIAO `D`-labels and must be updated to match.

| Net | Node pin | Connects to |
|---|---|---|
| LINK_TX | NODE_TX (UART to hub) | Hub RX (U2 -> U1.GPIO4, U3 -> U1.GPIO1) |
| LINK_RX | NODE_RX | Hub TX (U2 <- U1.GPIO5, U3 <- U1.GPIO2) |
| GPS_RX | NODE_GPS_RX (second UART RX) | U5.TX (the same GPS TX line goes to both nodes) |
| 5V | 5V | D1 (U2) / D2 (U3) cathode, via C2/C3 |
| GND | GND | GND |
| (unused) | all other pins, 3V3 | leave unconnected |

Firmware board targets: generic `ESP32C3 Dev Module` (USB CDC on boot enabled) and `ESP32H2 Dev Module`.

### 3.3 GPS U5

| U5 pin | Connects to |
|---|---|
| VCC | 3V3_HUB (through C12 10 uF + C10 100 nF to GND) — check your module accepts 3.3 V |
| GND | GND |
| TX | U2 NODE_GPS_RX **and** U3 NODE_GPS_RX |
| RX | not connected (leave a test pad; module is configured over USB-TTL beforehand) |

### 3.4 Display DISP1 (MSP2807-type, 14 pins) **[VERIFY pin order on your module]**

| Pin | Connects to |
|---|---|
| VCC | 3V3_HUB |
| GND | GND |
| CS | TFT_CS (GPIO10) |
| RESET | TFT_RST (GPIO8) |
| DC/RS | TFT_DC (GPIO9) |
| SDI (MOSI) | SPI_MOSI (GPIO11) |
| SCK | SPI_SCLK (GPIO12) |
| LED (backlight) | JP1 common pad (A -> 3V3_HUB, B -> TFT_BL GPIO7) |
| SDO (MISO) | **not connected** |
| T_CLK, T_CS, T_DIN, T_DO, T_IRQ | not connected |

### 3.5 CC1101 U4 **[VERIFY pin order]**

| Signal | Connects to |
|---|---|
| VCC | 3V3_HUB only (max 3.6 V — never 5 V) |
| GND | GND |
| SCK | SPI_SCLK |
| MOSI (SI) | SPI_MOSI |
| MISO (SO) | SPI_MISO |
| CSN | CC_CS (GPIO15) |
| GDO0 | CC_GDO0 (GPIO16) |
| GDO2 | CC_GDO2 (GPIO17) |

### 3.6 microSD J_SD

| Signal | Connects to |
|---|---|
| 3V3 | 3V3_HUB |
| GND | GND |
| CS | SD_CS (GPIO14) |
| MOSI | SPI_MOSI |
| MISO | SPI_MISO |
| SCK | SPI_SCLK |

### 3.7 Power stage PB1 (IP5310 power-bank board) **[VERIFY pads]**

| PB1 pad | Connects to |
|---|---|
| BAT+ / BAT- | BT1 18650 holder wires; BAT+ also to R1 (VBAT sense) |
| 5V OUT (+) | SW_PWR -> F1 (bridged) -> 5V_RAIL |
| GND (-) | GND |
| USB-C | charging input, reachable from the enclosure edge |
| USB-A | unused |

The on-board button wakes the IP5310 after a battery insert. Power-bank chips turn their output off below roughly 50-100 mA of load; the deck draws more than that, but check it doesn't shut down when idle.

### 3.8 Passives

| Ref | Between |
|---|---|
| SW_PWR | PB1 5V OUT and F1 |
| F1 | SW_PWR and 5V_RAIL (DNP, bridged) |
| C1 | 5V_RAIL to GND |
| D1, D2, D3 | anode 5V_RAIL, cathode to U2.5V / U3.5V / U1.5V |
| C2, C3, C4 | each module's 5V pin (after its diode) to GND |
| C5-C11 | 3V3/VCC of U1, U2, U3, DISP1, U4, U5, J_SD to GND, placed right at each pin |
| R1, R2 | PB1.BAT+ -> R1 -> VBAT_SENSE -> R2 -> GND |
| C13 | VBAT_SENSE to GND |
| R8, LED1 | 5V_RAIL -> R8 -> LED1 -> GND |
| SW_NAV | UP/DOWN/LEFT/RIGHT/MID to GPIO38-42 (see 3.1), COM to GND, SET/RST unconnected |
| J_SPARE | pin1 3V3_HUB, pin2 GND, pin3 GPIO18, pin4 GPIO21, pin5 GPIO48, pin6 5V_RAIL |

## 4. Layout rules (from the RF and power findings)

1. **Two layers, one solid ground plane** on the bottom layer. Do not split it.
2. **Keep-outs:** no copper, traces or components under the antenna end of U2/U3 (over each node's onboard antenna) or under the GPS patch antenna and its module.
3. **RF separation:** U2 and U3 at opposite short edges, ~10 cm apart. Both are receive-only, so this is a precaution.
4. **Placement:** hub U1 + DISP1 on the top face; U2, U3, U4, PB1 and the battery on the bottom face. GPS at the top edge with sky view. CC1101 antenna at a board edge, away from U2/U3 antennas.
5. **Power traces:** 1 mm minimum on 5V_RAIL, 1.5 mm from PB1 to C1. Put every bulk cap (C1-C4) close to the pin it feeds.
6. **USB-C access:** each module's USB-C must be reachable from the enclosure edge for flashing.
7. **SPI:** keep SCLK/MOSI short and away from the GPS. If the display SPI runs above ~40 MHz, add 33 Ohm series resistors on SCLK/MOSI (footprints only, DNP by default).

## 5. Open items before ordering boards
1. Footprint dimensions and pad order for every **[VERIFY]** item — do this from the datasheet or by measuring the physical part.
2. Measure real peak 5 V draw on the bench; switch to a 2 A boost module if > 900 mA.
3. Confirm the ESP32-H2 BLE scan works in Arduino-ESP32 3.x; fall back to a C3 SuperMini if not.
4. CC1101 band: 433 MHz (decided 2026-09-25 for SA).
5. Bench-test the IP5310: charge while running (dropout?), and idle auto-shutdown.
6. Decide whether the display backlight needs dimming; if so, replace JP1 with an AO3401 P-channel switch (not included here to keep the first spin simple).
