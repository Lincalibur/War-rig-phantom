#!/usr/bin/env python3
# Generates a KiCad 10 schematic for the SPECTRE HUB carrier/baseboard PCB.
# Reads real symbol defs from installed libs so pins are always correct.
#
# SPECTRE = ESP32-WROOM-32U + 3.5" ILI9488 8-bit PARALLEL TFT (UNO-form shield) + NEO-7M GPS.
# Pinout is the ACTUAL firmware wiring (spectre/v1/User_Setup_SPECTRE.h + spectre.ino), not the
# old ESP32-DIV/ILI9341 SPI plan. The TFT is modelled as the four Arduino-UNO shield headers so the
# physical mcufriend-style shield plugs straight in; the ESP32 GPIOs fan out to those header pins.
#
# Run:  python generate_schematic.py .      (needs KiCad 10 symbol libraries installed)
import re, uuid, os, sys

SYMDIR = r"C:\Program Files\KiCad\10.0\share\kicad\symbols"
OUTDIR = sys.argv[1] if len(sys.argv) > 1 else "."
PROJECT = "spectre"

def U():
    return str(uuid.uuid4())

# ---- read a top-level symbol block from a .kicad_sym file ----
def read_lib_file(lib):
    with open(os.path.join(SYMDIR, lib + ".kicad_sym"), encoding="utf-8") as f:
        return f.read()

def extract_symbol_block(text, name):
    marker = '\t(symbol "%s"' % name
    i = text.find(marker)
    if i < 0:
        raise SystemExit("symbol not found: " + name)
    depth = 0; k = i; instr = False; esc = False
    while k < len(text):
        c = text[k]
        if instr:
            if esc: esc = False
            elif c == '\\': esc = True
            elif c == '"': instr = False
        else:
            if c == '"': instr = True
            elif c == '(': depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return text[i:k+1]
        k += 1
    raise SystemExit("unterminated symbol: " + name)

def parse_pins(block):
    pins = {}
    for m in re.finditer(r'\(pin\s+\w+\s+\w+\s*\n\s*\(at\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+)\)', block):
        x, y = float(m.group(1)), float(m.group(2))
        nm = re.search(r'\(number\s+"([^"]+)"', block[m.end():])
        if nm:
            pins[nm.group(1)] = (x, y)
    return pins

def parse_pin_names(block):
    names = {}
    for pm in re.finditer(r'\(pin\s', block):
        seg = block[pm.start():pm.start()+400]
        nmn = re.search(r'\(name\s+"([^"]+)"', seg)
        nmnum = re.search(r'\(number\s+"([^"]+)"', seg)
        if nmn and nmnum:
            names[nmn.group(1)] = nmnum.group(1)
    return names

libcache = {}
def get_lib(lib):
    if lib not in libcache:
        libcache[lib] = read_lib_file(lib)
    return libcache[lib]

sym_blocks = {}; sym_pins = {}; sym_names = {}

def ensure_symbol(lib_id):
    if lib_id in sym_blocks:
        return
    lib, name = lib_id.split(":")
    text = get_lib(lib)
    block = extract_symbol_block(text, name)
    ext = re.search(r'\(extends\s+"([^"]+)"', block)
    if ext:
        parent = ext.group(1)
        pblock = extract_symbol_block(text, parent)
        flat = re.sub(r'^\t\(symbol\s+"%s"' % re.escape(parent),
                      '\t\t(symbol "%s"' % lib_id, pblock, count=1)
        flat = flat.replace('"%s_' % parent, '"%s_' % name)
        sym_blocks[lib_id] = flat
        sym_pins[lib_id] = parse_pins(pblock)
        sym_names[lib_id] = parse_pin_names(pblock)
    else:
        renamed = re.sub(r'^\t\(symbol\s+"%s"' % re.escape(name),
                         '\t\t(symbol "%s"' % lib_id, block, count=1)
        sym_blocks[lib_id] = renamed
        sym_pins[lib_id] = parse_pins(block)
        sym_names[lib_id] = parse_pin_names(block)

# ---------------- component definitions ----------------
NC = None
C  = "Connector_Generic:"
PH = "Connector_PinSocket_2.54mm:"
HD = "Connector_PinHeader_2.54mm:"

comps = []
def add(ref, lib_id, value, fp, pinmap, byname=False):
    comps.append(dict(ref=ref, lib=lib_id, val=value, fp=fp, pm=pinmap, byname=byname))

# =======================================================================================
# ESP32-WROOM-32U DevKitC-V4 dev board, socketed as two 1x19 female headers.
# Pin numbers = physical header positions in the canonical DevKitC-V4 order; the net on each
# pin = the signal carried by the GPIO that sits at that position.
#   !! VERIFY the pin order against your physical board (DevKitC variants differ) before fab. !!
# Flash pins IO6-IO11 are NOT usable (internal SPI flash). IO34/35/36/39 are INPUT-ONLY.
# =======================================================================================
add("J1", C+"Conn_01x19", "ESP32_WROOM_32U_L", PH+"PinSocket_1x19_P2.54mm_Vertical", {
 "1":"+3V3",      # 3V3  (board regulator OUT -> feeds TFT logic, GPS, expansion)
 "2":NC,          # EN
 "3":"EXP_IN3",   # IO36  input-only
 "4":"EXP_IN4",   # IO39  input-only
 "5":"EXP_IN1",   # IO34  input-only
 "6":"EXP_IN2",   # IO35  input-only
 "7":"TFT_RST",   # IO32
 "8":"TFT_CS",    # IO33
 "9":"TFT_D3",    # IO25
 "10":"TFT_D2",   # IO26
 "11":"TFT_D6",   # IO27
 "12":"TFT_D7",   # IO14
 "13":"TFT_D0",   # IO12  (STRAPPING: MTDI sets flash voltage - see README; optional pulldown R1)
 "14":"GND",      # GND
 "15":"TFT_D1",   # IO13
 "16":NC,         # IO9   flash - do not use
 "17":NC,         # IO10  flash - do not use
 "18":NC,         # IO11  flash - do not use
 "19":"+5V"})     # 5V / VIN  (board regulator IN)
add("J2", C+"Conn_01x19", "ESP32_WROOM_32U_R", PH+"PinSocket_1x19_P2.54mm_Vertical", {
 "1":"GND",       # GND
 "2":"EXP_MOSI",  # IO23  (VSPI MOSI - expansion)
 "3":"GPS_TX",    # IO22  ESP32 TX2 -> GPS RXD
 "4":NC,          # IO1   TX0 (USB serial) - leave free
 "5":NC,          # IO3   RX0 (USB serial) - leave free
 "6":"GPS_RX",    # IO21  ESP32 RX2 <- GPS TXD
 "7":"EXP_MISO",  # IO19  (VSPI MISO - expansion)
 "8":"EXP_SCK",   # IO18  (VSPI SCK  - expansion)
 "9":"EXP_CS0",   # IO5   (expansion chip-select; also a strapping pin, fine as output)
 "10":"TFT_D4",   # IO17
 "11":"TFT_D5",   # IO16
 "12":"TFT_WR",   # IO4
 "13":NC,         # IO0   boot strap - leave free
 "14":"TFT_RD",   # IO2   (STRAPPING - see README)
 "15":"TFT_DC",   # IO15  (STRAPPING - held by shield, usually fine)
 "16":NC,         # IO8   flash - do not use
 "17":NC,         # IO7   flash - do not use
 "18":NC,         # IO6   flash - do not use
 "19":"GND"})     # GND

# =======================================================================================
# 3.5" ILI9488 8-bit PARALLEL TFT, modelled as the four Arduino-UNO R3 shield headers.
# mcufriend-standard mapping: LCD_D0/D1 on Arduino D8/D9; LCD_D2..D7 on Arduino D2..D7;
# LCD_RD/WR/RS/CS/RST on A0..A4. The shield's 3V3 pin is an INPUT that powers the panel
# logic - it MUST be supplied (this was the white-screen root cause). Backlight = VCC (no pin).
# SD lines (D10-D13) are left unconnected (SD not wired in this build).
# =======================================================================================
add("J3", C+"Conn_01x08", "TFT_UNO_POWER", PH+"PinSocket_1x08_P2.54mm_Vertical", {
 "1":NC, "2":NC, "3":NC, "4":"+3V3",   # 3V3 -> TFT logic rail (REQUIRED)
 "5":"+5V", "6":"GND", "7":"GND", "8":NC})  # VIN unused
add("J4", C+"Conn_01x06", "TFT_UNO_ANALOG", PH+"PinSocket_1x06_P2.54mm_Vertical", {
 "1":"TFT_RD",   # A0 = LCD_RD
 "2":"TFT_WR",   # A1 = LCD_WR
 "3":"TFT_DC",   # A2 = LCD_RS/DC
 "4":"TFT_CS",   # A3 = LCD_CS
 "5":"TFT_RST",  # A4 = LCD_RST
 "6":NC})        # A5
add("J5", C+"Conn_01x08", "TFT_UNO_DIGITAL_LO", PH+"PinSocket_1x08_P2.54mm_Vertical", {
 "1":NC,         # D0/RX
 "2":NC,         # D1/TX
 "3":"TFT_D2",   # D2 = LCD_D2
 "4":"TFT_D3",   # D3 = LCD_D3
 "5":"TFT_D4",   # D4 = LCD_D4
 "6":"TFT_D5",   # D5 = LCD_D5
 "7":"TFT_D6",   # D6 = LCD_D6
 "8":"TFT_D7"})  # D7 = LCD_D7
add("J6", C+"Conn_01x10", "TFT_UNO_DIGITAL_HI", PH+"PinSocket_1x10_P2.54mm_Vertical", {
 "1":"TFT_D0",   # D8 = LCD_D0
 "2":"TFT_D1",   # D9 = LCD_D1
 "3":NC,         # D10 = SD_CS   (SD not wired)
 "4":NC,         # D11 = SD_MOSI
 "5":NC,         # D12 = SD_MISO
 "6":NC,         # D13 = SD_SCK
 "7":"GND",      # GND
 "8":NC,         # AREF
 "9":NC,         # SDA
 "10":NC})       # SCL

# =======================================================================================
# NEO-7M GPS (UART2). Header labels VCC/GND/TXD/RXD. GPS TXD -> ESP32 RX2 (net GPS_RX);
# GPS RXD <- ESP32 TX2 (net GPS_TX). VCC on +5V (module regulates); see README for 3V3 option.
# =======================================================================================
add("J7", C+"Conn_01x05", "NEO-7M_GPS", PH+"PinSocket_1x05_P2.54mm_Vertical", {
 "1":"+5V",      # VCC
 "2":"GND",      # GND
 "3":"GPS_RX",   # TXD (data INTO esp32 rx)
 "4":"GPS_TX",   # RXD (data FROM esp32 tx)
 "5":NC})        # PPS (unused)

# =======================================================================================
# EXPANSION header - the GPIOs the 8-bit parallel TFT leaves free. This is a VSPI bus
# (SCK/MOSI/MISO + one chip-select) plus 4 input-only lines and power. Enough for ONE SPI
# radio at a time (e.g. a single CC1101 or NRF24). The full 3xNRF24 + CC1101 shield does NOT
# fit alongside the parallel TFT - see README "GPIO budget".
# =======================================================================================
add("J8", C+"Conn_01x12", "EXPANSION", PH+"PinSocket_1x12_P2.54mm_Vertical", {
 "1":"+5V", "2":"+3V3", "3":"GND",
 "4":"EXP_SCK",   # IO18
 "5":"EXP_MOSI",  # IO23
 "6":"EXP_MISO",  # IO19
 "7":"EXP_CS0",   # IO5
 "8":"EXP_IN1",   # IO34 input-only (IRQ/GDO0)
 "9":"EXP_IN2",   # IO35 input-only (IRQ/GDO2)
 "10":"EXP_IN3",  # IO36 input-only
 "11":"EXP_IN4",  # IO39 input-only
 "12":"GND"})

# =======================================================================================
# Power input (5 V from the 1S 18650 + boost module). 5V -> ESP32 VIN; the ESP32 board's
# own 3V3 regulator then feeds +3V3. Optional separate LDO (not placed) if expansion radios
# brown out the board regulator - see README.
# =======================================================================================
add("J9", C+"Conn_01x02", "PWR_IN_5V", HD+"PinHeader_1x02_P2.54mm_Vertical", {
 "1":"+5V", "2":"GND"})

# Power LED + bulk + decoupling
add("R2", "Device:R", "1k", "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
    {"1":"+5V","2":"LED_A"})
add("D1", "Device:LED", "GREEN", "LED_THT:LED_D3.0mm", {"K":"GND","A":"LED_A"}, byname=True)

# Optional pulldown on IO12 (TFT_D0) strapping pin - fit only if boot/flash flakes. DNP.
add("R1", "Device:R", "10k_DNP", "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
    {"1":"TFT_D0","2":"GND"})

def cap(ref, val, fp, vnet):
    add(ref, "Device:C", val, fp, {"1":vnet,"2":"GND"})
CER = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
EL  = "Capacitor_THT:CP_Radial_D5.0mm_P2.50mm"
cap("C1","100nF",CER,"+3V3")   # ESP32
cap("C2","100nF",CER,"+5V")    # TFT 5V
cap("C3","100nF",CER,"+3V3")   # TFT logic
cap("C4","100nF",CER,"+5V")    # GPS
cap("C5","10uF", EL, "+5V")    # 5V bulk
cap("C6","10uF", EL, "+3V3")   # 3V3 bulk
cap("C7","100nF",CER,"+5V")    # expansion
cap("C8","10uF", EL, "+3V3")   # expansion radio bulk

# Power flags so ERC sees each supply net as driven
for i, net in enumerate(["+5V","+3V3","GND"], 1):
    add("#FLG%d" % i, "power:PWR_FLAG", net, "", {"1":net})

# ---------------- place & emit ----------------
for c in comps:
    ensure_symbol(c["lib"])

for c in comps:
    if c["byname"]:
        nm = sym_names[c["lib"]]
        for k in c["pm"]:
            if k not in nm:
                print("WARN: pin name %r not in %s; available=%s" % (k, c["lib"], sorted(nm)))

ROOT = "00000000-0000-0000-0000-000000000001"

def lbl(net, x, y, px):
    if net is None:
        return ""
    ang, just = (180, "right") if px < 0 else (0, "left")
    return ('\t(global_label "%s"\n\t\t(shape input)\n\t\t(at %.4f %.4f %d)\n'
            '\t\t(effects (font (size 1.27 1.27)) (justify %s))\n\t\t(uuid "%s")\n\t)\n'
            % (net, x, y, ang, just, U()))

def nocon(x, y):
    return '\t(no_connect (at %.4f %.4f) (uuid "%s"))\n' % (x, y, U())

col_x = [55, 150, 245, 340, 435, 530]
cur = {cx: 40.0 for cx in col_x}
body = []; labels = []
col_cycle = 0
for c in comps:
    pins = sym_pins[c["lib"]]
    names = sym_names[c["lib"]] if c["byname"] else None
    npin = len(pins)
    height = max(npin, 2) * 2.54 + 22
    cx = col_x[col_cycle % len(col_x)]
    if cur[cx] + height > 585:
        for k in range(len(col_x)):
            if cur[col_x[k]] + height <= 585:
                cx = col_x[k]; break
    def snap(v): return round(v / 1.27) * 1.27
    X = snap(float(cx)); Y = snap(cur[cx] + height/2.0)
    cur[cx] += height + 6; col_cycle += 1

    s = []
    s.append('\t(symbol')
    s.append('\t\t(lib_id "%s")' % c["lib"])
    s.append('\t\t(at %.4f %.4f 0)' % (X, Y))
    s.append('\t\t(unit 1)')
    s.append('\t\t(exclude_from_sim no)(in_bom yes)(on_board yes)(dnp no)')
    s.append('\t\t(uuid "%s")' % U())
    s.append('\t\t(property "Reference" "%s" (at %.4f %.4f 0)(effects (font (size 1.27 1.27))(justify left)))' % (c["ref"], X+3, Y - height/2 + 1))
    s.append('\t\t(property "Value" "%s" (at %.4f %.4f 0)(effects (font (size 1.27 1.27))(justify left)))' % (c["val"], X+3, Y - height/2 + 3))
    s.append('\t\t(property "Footprint" "%s" (at %.4f %.4f 0)(hide yes)(effects (font (size 1.27 1.27))))' % (c["fp"], X, Y))
    s.append('\t\t(property "Datasheet" "" (at %.4f %.4f 0)(hide yes)(effects (font (size 1.27 1.27))))' % (X, Y))
    for num in sorted(pins, key=lambda z:(len(z),z)):
        s.append('\t\t(pin "%s" (uuid "%s"))' % (num, U()))
    s.append('\t\t(instances (project "%s" (path "/%s" (reference "%s") (unit 1))))' % (PROJECT, ROOT, c["ref"]))
    s.append('\t)')
    body.append("\n".join(s) + "\n")

    for key, net in c["pm"].items():
        if c["byname"] and key not in names:
            print("SKIP %s: no pin named %s" % (c["ref"], key)); continue
        num = names[key] if c["byname"] else key
        if num not in pins:
            print("WARN %s: pin %s(%s) missing" % (c["ref"], key, num)); continue
        px, py = pins[num]
        sx, sy = X + px, Y - py
        labels.append(nocon(sx, sy) if net is None else lbl(net, sx, sy, px))

header = ('(kicad_sch\n\t(version 20250610)\n\t(generator "eeschema")\n\t(generator_version "10.0")\n'
          '\t(uuid "%s")\n\t(paper "A1")\n' % ROOT)
ls = ["\t(lib_symbols"]
for lib_id, blk in sym_blocks.items():
    ls.append(blk)
ls.append("\t)")
libsec = "\n".join(ls) + "\n"
footer = '\t(sheet_instances\n\t\t(path "/" (page "1"))\n\t)\n)\n'

out = header + libsec + "".join(body) + "".join(labels) + footer
path = os.path.join(OUTDIR, PROJECT + ".kicad_sch")
with open(path, "w", encoding="utf-8") as f:
    f.write(out)
print("wrote", path, "components:", len(comps))
