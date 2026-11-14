#!/usr/bin/env python3
# Generates the SPECTRE carrier .kicad_pcb: portrait outline + basic component PLACEMENT,
# with footprints and net assignments pulled from spectre.net. Routing is left to the user.
#
# Layout intent (user's spec):
#   - Portrait / vertical board (screen is vertical => long board).
#   - TFT shield on the FRONT (F.Cu), ESP32 dev board on the BACK (B.Cu) behind it.
#   - Antennas at the TOP (WiFi SMA + GPS module up top).
#
# Run with KiCad's bundled python:
#   "C:\Program Files\KiCad\10.0\bin\python.exe" generate_pcb.py
import pcbnew, re, os, sys

HERE   = os.path.dirname(os.path.abspath(__file__))
FPDIR  = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
NETF   = os.path.join(HERE, "spectre.net")
OUT    = os.path.join(HERE, "spectre.kicad_pcb")

def MM(v): return pcbnew.FromMM(v)
def V(x,y): return pcbnew.VECTOR2I(MM(x), MM(y))

# ---- parse spectre.net -> ref: {pad: net} ----
t = open(NETF, encoding="utf-8").read()
nets_sec = t[t.index('(nets'):]
padmap = {}
for b in re.split(r'\(code "\d+"\)\s*\(name "', nets_sec)[1:]:
    name = b[:b.index('"')]
    for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"', b):
        padmap.setdefault(ref, {})[pin] = name

# ---- footprint ids (match the schematic) ----
FP = {
 "J1":"Connector_PinSocket_2.54mm:PinSocket_1x19_P2.54mm_Vertical",
 "J2":"Connector_PinSocket_2.54mm:PinSocket_1x19_P2.54mm_Vertical",
 "J3":"Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
 "J4":"Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
 "J5":"Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
 "J6":"Connector_PinSocket_2.54mm:PinSocket_1x10_P2.54mm_Vertical",
 "J7":"Connector_PinSocket_2.54mm:PinSocket_1x05_P2.54mm_Vertical",
 "J8":"Connector_PinSocket_2.54mm:PinSocket_1x12_P2.54mm_Vertical",
 "J9":"Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
 "R1":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "R2":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "D1":"LED_THT:LED_D3.0mm",
 "C1":"Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
 "C2":"Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
 "C3":"Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
 "C4":"Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
 "C5":"Capacitor_THT:CP_Radial_D5.0mm_P2.50mm",
 "C6":"Capacitor_THT:CP_Radial_D5.0mm_P2.50mm",
 "C7":"Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
 "C8":"Capacitor_THT:CP_Radial_D5.0mm_P2.50mm",
}

# ---- placement table: ref -> (x_mm, y_mm, rotation_deg, side) ; side 'F' or 'B' ----
# Board is 70 x 130 mm, portrait. Origin top-left, +Y down.
# NOTE: these PinSocket footprints default to pads stacked ALONG Y (vertical) at rot 0;
# rot 90 lays them horizontal. The TFT's two header groups run vertically down the long
# side edges (so the UNO shield mates); GPS + expansion lie horizontal at top/bottom.
# ESP32 rows on the BACK sit in the central gap, directly behind the screen.
PLACE = {
 # --- TOP: antennas + indicator ---
 "ANT1":(14, 9,  0, "F"),    # WiFi SMA (mechanical, no net)
 "J7":  (45,14, 90, "F"),    # NEO-7M GPS, horizontal, up top
 "D1":  (61,10,  0, "F"),    # power LED
 "R2":  (61,17, 90, "F"),
 # --- MIDDLE: TFT shield headers on FRONT (vertical, rot 0), two columns ~48mm apart ---
 "J4":  (11,48,  0, "F"),    # analog  A0-A5     (left col, top)
 "J3":  (11,78,  0, "F"),    # power   8-pin     (left col, bottom)
 "J6":  (59,52,  0, "F"),    # digital-hi D8-D13 (right col, top)
 "J5":  (59,85,  0, "F"),    # digital-lo D2-D7  (right col, bottom)
 # --- MIDDLE: ESP32 DevKit on BACK (two 1x19 rows, vertical), in the central gap ---
 "J1":  (30,64,  0, "B"),    # left header row
 "J2":  (50,64,  0, "B"),    # right header row (~20mm apart)
 # --- BOTTOM: expansion, power, bulk ---
 "J8":  (35,104,90, "F"),    # expansion 1x12, horizontal
 "J9":  (9, 122, 0, "F"),    # 5V power in
 # decoupling / bulk along the bottom, front
 "C1":  (16,113,90,"F"), "C2":(24,113,90,"F"), "C3":(32,113,90,"F"), "C4":(40,113,90,"F"),
 "C7":  (48,113,90,"F"),
 "C5":  (16,122,90,"F"), "C6":(24,122,90,"F"), "C8":(32,122,90,"F"),
 "R1":  (58,113,90,"F"),     # DNP IO12 pulldown
}

board = pcbnew.CreateEmptyBoard()

netinfo = {}
def get_net(name):
    if name not in netinfo:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni
    return netinfo[name]

def load_fp(fpid):
    lib, name = fpid.split(":")
    fp = pcbnew.FootprintLoad(os.path.join(FPDIR, lib + ".pretty"), name)
    if fp is None:
        raise SystemExit("footprint not found: " + fpid)
    return fp

placed = []
# --- real (netted) components ---
for ref, fpid in FP.items():
    if ref not in PLACE:
        print("no placement for", ref); continue
    x, y, rot, side = PLACE[ref]
    fp = load_fp(fpid)
    fp.SetReference(ref)
    board.Add(fp)
    fp.SetPosition(V(x, y))
    if side == "B":
        fp.Flip(V(x, y), False)
    fp.SetOrientationDegrees(rot)
    for pad in fp.Pads():
        net = padmap.get(ref, {}).get(pad.GetNumber())
        if net:
            pad.SetNet(get_net(net))
    placed.append(ref)

# --- mechanical: WiFi SMA + 4 mounting holes (no nets) ---
sma = load_fp("Connector_Coaxial:SMA_Amphenol_132134_Vertical")
sma.SetReference("ANT1")
board.Add(sma); sma.SetPosition(V(*PLACE["ANT1"][:2]))
placed.append("ANT1")

for i,(hx,hy) in enumerate([(5,5),(65,5),(5,125),(65,125)], 1):
    h = load_fp("MountingHole:MountingHole_3.2mm_M3")
    h.SetReference("H%d"%i); board.Add(h); h.SetPosition(V(hx,hy))

# ---- board outline on Edge.Cuts (portrait rectangle, 70 x 130) ----
W, Hh = 70.0, 130.0
def edge(x1,y1,x2,y2):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(x1,y1)); s.SetEnd(V(x2,y2))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(MM(0.15))
    board.Add(s)
edge(0,0,W,0); edge(W,0,W,Hh); edge(W,Hh,0,Hh); edge(0,Hh,0,0)

# ---- silk labels for the antenna zone ----
def text(txt,x,y,size=1.2,layer=None):
    tx = pcbnew.PCB_TEXT(board)
    tx.SetText(txt); tx.SetPosition(V(x,y))
    tx.SetLayer(layer if layer is not None else pcbnew.F_SilkS)
    tx.SetTextSize(pcbnew.VECTOR2I(MM(size),MM(size)))
    board.Add(tx)
text("WiFi ANT", 14, 3)
text("GPS (up)", 46, 4)
text("SPECTRE HUB  (TFT front / ESP32 back)", 35, 128, 1.0)

pcbnew.SaveBoard(OUT, board)
print("wrote", OUT)
print("placed:", ", ".join(placed))
print("nets:", len(netinfo))
