# ESP32-DIV HUB — KiCad carrier PCB (schematic pre-built)

A starting point for routing a custom carrier/shield PCB for the **HUB** board
(the ESP32-DIV handheld in [`../../docs/hardware-layout.md`](../../docs/hardware-layout.md)).
Every plug-in module is a female-header footprint; the ESP32 dev board and all
modules socket in. This does **not** change the design — it just captures the
existing HUB pinout in KiCad so you can place and route.

Built and validated against **KiCad 10.0.6**. Netlist checked pin-by-pin; ERC
clean except one benign warning (see below).

## Files
| File | What it is |
|---|---|
| `esp32div.kicad_sch` | The schematic: 39 symbols, all nets labelled, ready for PCB. |
| `esp32div.kicad_pro` | Project file. Includes a **Power** netclass (1 mm tracks) auto-applied to `+5V`, `+3V3`, `GND`, `VBAT`, `VBOOST`. |
| `generate_schematic.py` | The generator. Edit the pin maps and re-run (`python generate_schematic.py .`) to regenerate the schematic. |

## Go straight to routing
1. Open `esp32div.kicad_pro` in KiCad.
2. Open the **PCB Editor** (it creates `esp32div.kicad_pcb`).
3. **Tools → Update PCB from Schematic** (F8) → *Update*. All 39 footprints drop
   in with the full ratsnest.
4. Draw a board outline on Edge.Cuts, then place and route. The power nets are
   already on 1 mm tracks via the Power netclass.

## The one ERC warning
`lib_symbol_mismatch` on `PCF8574T` — the PCF8574 is a *derived* library symbol,
so the cached copy isn't byte-identical to the library. It's cosmetic and does
**not** affect the netlist or PCB. To clear it: **Tools → Update Symbols from
Library** in the schematic editor.

## ⚠️ Verify before you fab (modelled from docs, not physical parts)
- **ESP32 pin order (most important).** `J1`/`J2` are modelled as two 1×19
  headers using the standard **38-pin DevKitC-V4** order. Confirm against your
  actual WROOM-32U board when it arrives. If a pin differs, just drag the net
  label to the correct pin (or edit the pin map in `generate_schematic.py` and
  re-run). The board can't be finalised until the dev-board variant is confirmed
  (see the open question in `hardware-layout.md`).
- **TFT header (`J3`).** Modelled as one 18-way header with the full
  TFT+touch+SD signal set. Match pin order/count to your physical 2.4" module.
- **CC1101 (`J8`) and NRF24 (`J5–J7`) pin order.** Vendor-dependent; check the
  silkscreen on your modules. NRF24 uses a 2×4 socket footprint.
- **VCC rails.** TFT and GPS VCC are on `+5V` per the docs (both have on-board
  regulators). Confirm your modules accept 5 V. PCF8574, NRF24, CC1101 are on
  `+3V3`.

## Design notes carried over from the docs
- Two SPI buses: display pixels on `TFT_*` (IO14/13/12), everything else (touch,
  SD, 3× NRF24, CC1101) on the shared `SH_*` bus (IO18/23/19).
- Intentionally **shared** control nets (cifertech firmware time-shares them):
  `SD_CS_NRF1_CSN` (IO5), `NRF2_CE_CC_GDO0` (IO26), `NRF2_CSN_CC_CS` (IO27),
  `NRF3_CE_CC_GDO2` (IO16). These fan out to several module pins by design.
- Decoupling: 100 nF at every module VCC; extra 10 µF at each NRF24, the CC1101
  and the GPS. Keep them at the pins when you place.
- Consider the optional separate 3V3 LDO for the radios (not placed) if the dev
  board's regulator browns out under 3× NRF24 + CC1101.
