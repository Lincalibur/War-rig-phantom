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
 "J3":"Module:Arduino_UNO_R3",   # TFT UNO shield mates here (exact Arduino geometry)
 "J7":"Connector_PinSocket_2.54mm:PinSocket_1x05_P2.54mm_Vertical",
 "J8":"Connector_PinSocket_2.54mm:PinSocket_1x12_P2.54mm_Vertical",
 "J9":"Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
 "R1":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "R2":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "R3":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "R4":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
 "R5":"Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
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
# Board is 75 x 150 mm, portrait. Origin top-left, +Y down. Mounting holes at the corners
# are kept clear. These PinSocket footprints default to pads stacked ALONG Y (vertical) at
# rot 0; rot 90 lays them horizontal. The TFT's two header groups run vertically down the
# long side edges (so the UNO shield mates, ~48 mm apart); GPS + expansion lie horizontal.
# ESP32 rows on the BACK sit in the central gap (x 28 & 51), directly behind the screen,
# offset from the TFT header columns (x 13 & 61) so no through-hole pads collide.
PLACE = {
 # --- TOP: antennas + indicator ---
 "ANT1":(17,15,  0, "F"),    # WiFi SMA (mechanical, no net)
 "J7":  (42,15, 90, "F"),    # NEO-7M GPS, horizontal, up top
 "D1":  (60,13,  0, "F"),    # power LED
 "R2":  (60,21,  0, "F"),    # LED series R, horizontal
 # --- MIDDLE: TFT = one Arduino UNO shield footprint on FRONT, rotated 90 (screen vertical),
 #     recentred to (CX, 65). Its two header rows land at x ~13 and ~61 (48 mm apart). ---
 "J3":  (37.5,65, 90, "F"),
 # --- MIDDLE: ESP32 DevKit on BACK (two 1x19 rows, vertical), central gap, ~23 mm apart ---
 "J1":  (28,70,  0, "B"),    # left header row
 "J2":  (51,70,  0, "B"),    # right header row
 # --- BOTTOM: expansion, then two tidy rows of passives ---
 "J8":  (37,106,90, "F"),    # expansion 1x12, horizontal
 "C1":  (14,120,0,"F"), "C2":(25,120,0,"F"), "C3":(36,120,0,"F"), "C4":(47,120,0,"F"),
 "C7":  (58,120,0,"F"),
 "C5":  (14,133,0,"F"), "C6":(25,133,0,"F"), "C8":(36,133,0,"F"),
 "R1":  (49,133,0,"F"),      # IO12 (EXP_MOSI) pulldown, horizontal
 "J9":  (60,133,0,"F"),      # 5V power in
 # boot pull-ups (10k -> 3V3): EXP_CS0, TFT_CS, TFT_RD
 "R3":  (14,140,0,"F"), "R4":(30,140,0,"F"), "R5":(46,140,0,"F"),
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

# ---- short, readable label for a net (for the silk pin names) ----
def short(net):
    if net is None: return None
    return (net.replace("TFT_","").replace("EXP_","").replace("GPS_","G")
               .replace("+3V3","3V3").replace("+5V","5V").replace("LED_A","LED"))

# ---- add a small net-name label next to each netted pad, on the given silk layer ----
def label_pads(fp, side, mode, off=1.4, names=None):
    # mode: 'left'/'right' (vertical headers) or 'up'/'down' (horizontal) or 'auto' (2 columns)
    # off : how far (mm) the label sits to the side of the pad; bump it up for fat
    #       socket bodies (e.g. the ESP32 1x19 sockets) so the text clears the body.
    # names: optional {pad_number: text}. If given, label each pad with that text (e.g. the
    #       ESP32's own GPIO names) instead of the net name, so the header reads like the
    #       physical dev board rather than echoing the shield's signal names.
    layer = pcbnew.B_SilkS if side == "B" else pcbnew.F_SilkS
    cx = fp.GetPosition().x
    OFF = MM(off)
    for pad in fp.Pads():
        if names is not None:
            nm = names.get(int(pad.GetNumber()))
            if not nm:
                continue
        else:
            raw = pad.GetNetname()
            if not raw or raw.startswith("unconnected") or raw.startswith("Net-"):
                continue
            nm = short(raw)
        p = pad.GetPosition()
        m = mode
        if mode == "auto":
            m = "left" if p.x < cx else "right"
        dx, dy, rotT, just = 0, 0, 0, pcbnew.GR_TEXT_H_ALIGN_LEFT
        if m == "left":  dx, just = -OFF, pcbnew.GR_TEXT_H_ALIGN_RIGHT
        elif m == "right": dx, just = OFF, pcbnew.GR_TEXT_H_ALIGN_LEFT
        elif m == "up":    dy, rotT, just = -OFF, 90, pcbnew.GR_TEXT_H_ALIGN_LEFT
        elif m == "down":  dy, rotT, just = OFF, 90, pcbnew.GR_TEXT_H_ALIGN_RIGHT
        # Back-silk text is mirrored, which visually inverts horizontal justify and would
        # make the label grow back UNDER the socket body. Flip it so it extends outward.
        if side == "B" and m in ("left", "right"):
            just = (pcbnew.GR_TEXT_H_ALIGN_LEFT if just == pcbnew.GR_TEXT_H_ALIGN_RIGHT
                    else pcbnew.GR_TEXT_H_ALIGN_RIGHT)
        tx = pcbnew.PCB_TEXT(board)
        tx.SetText(nm); tx.SetPosition(pcbnew.VECTOR2I(p.x+dx, p.y+dy))
        tx.SetLayer(layer)
        tx.SetTextSize(pcbnew.VECTOR2I(MM(0.7), MM(0.7)))
        tx.SetTextThickness(MM(0.12))
        tx.SetHorizJustify(just)
        tx.SetTextAngleDegrees(rotT)
        if side == "B": tx.SetMirrored(True)
        board.Add(tx)

# which connectors get pin-name labels, on which side, and (optional) how far out.
# J1/J2 are the ESP32 1x19 SOCKETS: their body is wide, so push the labels 2.6 mm out
# to sit clearly beside the pins instead of tucked under the socket body.
LABEL = {"J1":("B","left",2.6), "J2":("B","right",2.6), "J3":("F","auto"),
         "J7":("F","up"), "J8":("F","down"), "J9":("F","right")}

# ESP32-WROOM-32U DevKitC-V4 GPIO names, by socket pad number (canonical dev-board order).
# Used to label J1/J2 with the ESP32's OWN pin names, not the net/shield signal names.
ESP32_GPIO = {
 "J1": {1:"3V3",2:"EN",3:"IO36",4:"IO39",5:"IO34",6:"IO35",7:"IO32",8:"IO33",9:"IO25",
        10:"IO26",11:"IO27",12:"IO14",13:"IO12",14:"GND",15:"IO13",16:"IO9",17:"IO10",
        18:"IO11",19:"5V"},
 "J2": {1:"GND",2:"IO23",3:"IO22",4:"TX0",5:"RX0",6:"IO21",7:"IO19",8:"IO18",9:"IO5",
        10:"IO17",11:"IO16",12:"IO4",13:"IO0",14:"IO2",15:"IO15",16:"IO8",17:"IO7",
        18:"IO6",19:"GND"},
}

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
    # The Arduino_UNO_R3 footprint draws a big board OUTLINE (silk/fab/courtyard) that
    # just clutters the carrier. Strip its graphic shapes but keep the pads (holes) and
    # our pin-name labels. Done before recentre so the recenter uses the pad array only.
    if ref == "J3":
        for it in list(fp.GraphicalItems()):
            if isinstance(it, pcbnew.PCB_SHAPE):
                fp.Remove(it)
        # recenter so pads sit symmetric about (x, y)
        bb = fp.GetBoundingBox(False, False)
        ctr = bb.GetCenter()
        fp.Move(pcbnew.VECTOR2I(V(x, y).x - ctr.x, V(x, y).y - ctr.y))
    for pad in fp.Pads():
        net = padmap.get(ref, {}).get(pad.GetNumber())
        if net:
            pad.SetNet(get_net(net))
    # hide the long value label (clutter); keep a small reference designator
    fp.Value().SetVisible(False)
    fp.Reference().SetTextSize(pcbnew.VECTOR2I(MM(1.0), MM(1.0)))
    if ref in LABEL:
        label_pads(fp, *LABEL[ref], names=ESP32_GPIO.get(ref))
    placed.append(ref)

# --- mechanical: WiFi SMA + 4 mounting holes (no nets) ---
sma = load_fp("Connector_Coaxial:SMA_Amphenol_132134_Vertical")
sma.SetReference("ANT1")
board.Add(sma); sma.SetPosition(V(*PLACE["ANT1"][:2]))
sma.Value().SetVisible(False)
placed.append("ANT1")

for i,(hx,hy) in enumerate([(6,6),(69,6),(6,144),(69,144)], 1):
    h = load_fp("MountingHole:MountingHole_3.2mm_M3")
    h.SetReference("H%d"%i); board.Add(h); h.SetPosition(V(hx,hy))
    h.Value().SetVisible(False)

# ---- board outline on Edge.Cuts (portrait rectangle, 75 x 150) ----
W, Hh = 75.0, 150.0
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
text("WiFi ANT", 17, 7, 1.0)
text("GPS (up)", 42, 8, 1.0)
text("SPECTRE HUB", 37, 33, 1.6)
text("TFT front / ESP32 back", 37, 38, 1.0)

# SaveBoard rewrites the sibling .kicad_pro and strips our Power netclass / ERC settings,
# so preserve it across the save.
PRO = os.path.join(HERE, "spectre.kicad_pro")
pro_backup = open(PRO, encoding="utf-8").read() if os.path.exists(PRO) else None
pcbnew.SaveBoard(OUT, board)
if pro_backup is not None:
    open(PRO, "w", encoding="utf-8").write(pro_backup)
print("wrote", OUT)
print("placed:", ", ".join(placed))
print("nets:", len(netinfo))
