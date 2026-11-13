#!/usr/bin/env python3
# Generates a KiCad 10 schematic for the ESP32-DIV HUB carrier board.
# Reads real symbol defs from installed libs so pins are always correct.
import re, uuid, os, sys

SYMDIR = r"C:\Program Files\KiCad\10.0\share\kicad\symbols"
OUTDIR = sys.argv[1] if len(sys.argv) > 1 else "."
PROJECT = "esp32div"

def U():
    return str(uuid.uuid4())

# ---- read a top-level symbol block from a .kicad_sym file ----
def read_lib_file(lib):
    with open(os.path.join(SYMDIR, lib + ".kicad_sym"), encoding="utf-8") as f:
        return f.read()

def extract_symbol_block(text, name):
    # find '(symbol "name"' at one-tab indent, capture to matching paren
    marker = '\t(symbol "%s"' % name
    i = text.find(marker)
    if i < 0:
        raise SystemExit("symbol not found: " + name)
    depth = 0
    k = i
    instr = False
    esc = False
    while k < len(text):
        c = text[k]
        if instr:
            if esc:
                esc = False
            elif c == '\\':
                esc = True
            elif c == '"':
                instr = False
        else:
            if c == '"':
                instr = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return text[i:k+1]
        k += 1
    raise SystemExit("unterminated symbol: " + name)

# parse pins: number -> (x, y)
def parse_pins(block):
    pins = {}
    for m in re.finditer(r'\(pin\s+\w+\s+\w+\s*\n\s*\(at\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+)\)', block):
        x, y = float(m.group(1)), float(m.group(2))
        nm = re.search(r'\(number\s+"([^"]+)"', block[m.end():])
        if nm:
            pins[nm.group(1)] = (x, y)
    return pins

# parse pin name->number map
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

sym_blocks = {}   # lib_id -> renamed block text (for lib_symbols)
sym_pins = {}     # lib_id -> {num:(x,y)}
sym_names = {}    # lib_id -> {name:num}

def ensure_symbol(lib_id):
    if lib_id in sym_blocks:
        return
    lib, name = lib_id.split(":")
    text = get_lib(lib)
    block = extract_symbol_block(text, name)
    # handle 'extends' (derived symbol) -> flatten into parent geometry
    ext = re.search(r'\(extends\s+"([^"]+)"', block)
    if ext:
        parent = ext.group(1)
        pblock = extract_symbol_block(text, parent)
        # embed parent's full geometry under the derived lib_id
        flat = re.sub(r'^\t\(symbol\s+"%s"' % re.escape(parent),
                      '\t\t(symbol "%s"' % lib_id, pblock, count=1)
        # rename inner unit sub-symbols PARENT_x_y -> NAME_x_y
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
# each: ref, lib_id, value, footprint, pinmap {pinnumber_or_name: net or None}
NC = None
C = "Connector_Generic:"
PH = "Connector_PinSocket_2.54mm:"

comps = []
def add(ref, lib_id, value, fp, pinmap, byname=False):
    comps.append(dict(ref=ref, lib=lib_id, val=value, fp=fp, pm=pinmap, byname=byname))

# ESP32-WROOM-32U dev board, 38-pin DevKitC-V4 order, as two 1x19 header sockets
add("J1", C+"Conn_01x19", "ESP32_DEVKIT_L", PH+"PinSocket_1x19_P2.54mm_Vertical", {
 "1":"+3V3","2":NC,"3":NC,"4":NC,"5":NC,"6":NC,"7":"TFT_BL","8":"T_CS","9":NC,
 "10":"NRF2_CE_CC_GDO0","11":"NRF2_CSN_CC_CS","12":"TFT_SCK","13":"TFT_MISO",
 "14":"GND","15":"TFT_MOSI","16":NC,"17":NC,"18":NC,"19":"+5V"})
add("J2", C+"Conn_01x19", "ESP32_DEVKIT_R", PH+"PinSocket_1x19_P2.54mm_Vertical", {
 "1":"GND","2":"SH_MOSI","3":"SCL","4":"GPS_RXD","5":"GPS_TXD","6":"SDA","7":"GND",
 "8":"SH_MISO","9":"SH_SCK","10":"SD_CS_NRF1_CSN","11":"NRF3_CSN","12":"NRF3_CE_CC_GDO2",
 "13":"NRF1_CE","14":"TFT_RST","15":"TFT_DC","16":"TFT_CS","17":NC,"18":NC,"19":NC})

# 2.4" ILI9341 TFT + XPT2046 touch + SD, single 18-way header
add("J3", C+"Conn_01x18", "ILI9341_TFT_TOUCH_SD", PH+"PinSocket_1x18_P2.54mm_Vertical", {
 "1":"+5V","2":"GND","3":"TFT_CS","4":"TFT_RST","5":"TFT_DC","6":"TFT_MOSI","7":"TFT_SCK",
 "8":"TFT_BL","9":"TFT_MISO","10":"SH_SCK","11":"T_CS","12":"SH_MOSI","13":"SH_MISO",
 "14":NC,"15":"SD_CS_NRF1_CSN","16":"SH_MOSI","17":"SH_MISO","18":"SH_SCK"})

# PCF8574 I/O expander (map by pin NAME)
add("U2", "Interface_Expansion:PCF8574T", "PCF8574", "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm", {
 "A0":"GND","A1":"GND","A2":"GND","GND":"GND","VDD":"+3V3","SDA":"SDA","SCL":"SCL","~{INT}":NC,
 "P0":NC,"P1":NC,"P2":NC,"P3":"BTN_DOWN","P4":"BTN_LEFT","P5":"BTN_RIGHT","P6":"BTN_UP","P7":"BTN_SEL"},
 byname=True)

# 5 nav buttons -> PCF pin to GND
for ref, net in [("SW1","BTN_UP"),("SW2","BTN_DOWN"),("SW3","BTN_LEFT"),("SW4","BTN_RIGHT"),("SW5","BTN_SEL")]:
    add(ref, "Switch:SW_Push", "BTN_"+net.split("_")[1], "Button_Switch_THT:SW_PUSH_6mm",
        {"1":net,"2":"GND"})

# NEO-7M GPS
add("J4", C+"Conn_01x04", "NEO-7M_GPS", PH+"PinSocket_1x04_P2.54mm_Vertical", {
 "1":"+5V","2":"GND","3":"GPS_TXD","4":"GPS_RXD"})

# NRF24L01+ x3  (1x8 symbol, 2x4 socket footprint)
add("J5", C+"Conn_01x08", "NRF24L01_1", PH+"PinSocket_2x04_P2.54mm_Vertical", {
 "1":"GND","2":"+3V3","3":"NRF1_CE","4":"SD_CS_NRF1_CSN","5":"SH_SCK","6":"SH_MOSI","7":"SH_MISO","8":NC})
add("J6", C+"Conn_01x08", "NRF24L01_2", PH+"PinSocket_2x04_P2.54mm_Vertical", {
 "1":"GND","2":"+3V3","3":"NRF2_CE_CC_GDO0","4":"NRF2_CSN_CC_CS","5":"SH_SCK","6":"SH_MOSI","7":"SH_MISO","8":NC})
add("J7", C+"Conn_01x08", "NRF24L01_3", PH+"PinSocket_2x04_P2.54mm_Vertical", {
 "1":"GND","2":"+3V3","3":"NRF3_CE_CC_GDO2","4":"NRF3_CSN","5":"SH_SCK","6":"SH_MOSI","7":"SH_MISO","8":NC})

# CC1101 sub-GHz (1x8)
add("J8", C+"Conn_01x08", "CC1101_433", PH+"PinSocket_1x08_P2.54mm_Vertical", {
 "1":"+3V3","2":"GND","3":"SH_MOSI","4":"SH_SCK","5":"SH_MISO","6":"NRF3_CE_CC_GDO2",
 "7":"NRF2_CE_CC_GDO0","8":"NRF2_CSN_CC_CS"})

# Power input chain
add("J9", C+"Conn_01x02", "18650_PACK", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
    {"1":"VBAT","2":"GND"})
add("J10", C+"Conn_01x04", "BOOST_5V_2A", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
    {"1":"VBAT","2":"GND","3":"VBOOST","4":"GND"})
add("SW6", C+"Conn_01x03", "PWR_SLIDE_SW", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
    {"1":"VBOOST","2":"+5V","3":NC})

# Indicator + bulk
add("R1", "Device:R", "1k", "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
    {"1":"+5V","2":"LED_A"})
add("D1", "Device:LED", "GREEN", "LED_THT:LED_D3.0mm", {"K":"GND","A":"LED_A"}, byname=True)
add("C1", "Device:C_Polarized", "470uF", "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm", {"1":"+5V","2":"GND"})

# Decoupling
def cap(ref, val, fp, vnet):
    add(ref, "Device:C", val, fp, {"1":vnet,"2":"GND"})
CER = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
EL  = "Capacitor_THT:CP_Radial_D5.0mm_P2.50mm"
cap("C2","100nF",CER,"+3V3")   # ESP32
cap("C3","100nF",CER,"+5V")    # TFT
cap("C4","100nF",CER,"+3V3")   # PCF
cap("C5","100nF",CER,"+5V")    # GPS
cap("C6","10uF",EL,"+5V")      # GPS bulk
cap("C7","100nF",CER,"+3V3")   # NRF1
cap("C8","10uF",EL,"+3V3")     # NRF1 bulk
cap("C9","100nF",CER,"+3V3")   # NRF2
cap("C10","10uF",EL,"+3V3")    # NRF2 bulk
cap("C11","100nF",CER,"+3V3")  # NRF3
cap("C12","10uF",EL,"+3V3")    # NRF3 bulk
cap("C13","100nF",CER,"+3V3")  # CC1101
cap("C14","10uF",EL,"+3V3")    # CC1101 bulk
cap("C15","10uF",EL,"+5V")     # 5V bulk

# Power flags so ERC sees each supply net as driven
for i, net in enumerate(["+5V","+3V3","GND","VBAT","VBOOST"], 1):
    add("#FLG%d" % i, "power:PWR_FLAG", net, "", {"1":net})

# ---------------- place & emit ----------------
for c in comps:
    ensure_symbol(c["lib"])

# verify name maps for byname comps
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
    if px < 0:
        ang, just = 180, "right"
    elif px > 0:
        ang, just = 0, "left"
    else:
        ang, just = 0, "left"
    return ('\t(global_label "%s"\n\t\t(shape input)\n\t\t(at %.4f %.4f %d)\n'
            '\t\t(effects (font (size 1.27 1.27)) (justify %s))\n\t\t(uuid "%s")\n\t)\n'
            % (net, x, y, ang, just, U()))

def nocon(x, y):
    return '\t(no_connect (at %.4f %.4f) (uuid "%s"))\n' % (x, y, U())

# layout grid
col_x = [55, 150, 245, 340, 435, 530]
slot_h = {}  # dynamic per column
cur = {cx: 40.0 for cx in col_x}
ci = 0

body = []
labels = []

# place: big connectors first across columns, then passives
order = comps
col_cycle = 0
for c in order:
    pins = sym_pins[c["lib"]]
    names = sym_names[c["lib"]] if c["byname"] else None
    npin = len(pins)
    height = max(npin, 2) * 2.54 + 22
    cx = col_x[col_cycle % len(col_x)]
    if cur[cx] + height > 585:
        # find a column with room
        placed = False
        for k in range(len(col_x)):
            cx2 = col_x[k]
            if cur[cx2] + height <= 585:
                cx = cx2; placed = True; break
        if not placed:
            cx = col_x[col_cycle % len(col_x)]
    def snap(v): return round(v / 1.27) * 1.27
    X = snap(float(cx))
    Y = snap(cur[cx] + height/2.0)
    cur[cx] += height + 6
    col_cycle += 1

    # symbol instance
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

    # labels / no-connects
    if c["byname"]:
        print("NAMES %s(%s): %s" % (c["ref"], c["lib"], sorted(names.keys())))
    for key, net in c["pm"].items():
        if c["byname"] and key not in names:
            print("SKIP %s: no pin named %s" % (c["ref"], key)); continue
        num = names[key] if c["byname"] else key
        if num not in pins:
            print("WARN %s: pin %s(%s) missing" % (c["ref"], key, num)); continue
        px, py = pins[num]
        sx, sy = X + px, Y - py
        if net is None:
            labels.append(nocon(sx, sy))
        else:
            labels.append(lbl(net, sx, sy, px))

# assemble file
header = ('(kicad_sch\n\t(version 20250610)\n\t(generator "eeschema")\n\t(generator_version "10.0")\n'
          '\t(uuid "%s")\n\t(paper "A1")\n' % ROOT)
libsec = "\t(lib_symbols\n" + "".join("\t"+b.replace("\n","\n\t").rstrip("\t") for b in [])  # placeholder
# build lib_symbols from collected blocks
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
