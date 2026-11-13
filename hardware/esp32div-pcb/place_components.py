import pcbnew, sys

PCB = r"C:\Users\liamo\Desktop\Ideas\Cyber security\CyberDeck\hardware\esp32div-pcb\esp32div.kicad_pcb"
W, H = 70.0, 125.0          # board size mm
MARGIN = 3.0                # keep parts this far from edge
GAP = 0.8                   # clearance added around each part (mm)

b = pcbnew.LoadBoard(PCB)
fps = {fp.GetReference(): fp for fp in b.GetFootprints()}
print("footprints on board:", len(fps))

# normalise: everything back to front first, so courtyards are on F_CrtYd
for fp in fps.values():
    if fp.IsFlipped():
        fp.Flip(fp.GetPosition(), False)

def mm(v): return pcbnew.FromMM(v)// 1
def V(x, y): return pcbnew.VECTOR2I(int(mm(x)), int(mm(y)))

def pad_center(fp):
    x1 = y1 = 1e9; x2 = y2 = -1e9
    for p in fp.Pads():
        pb = p.GetBoundingBox()
        x1 = min(x1, pb.GetLeft());  y1 = min(y1, pb.GetTop())
        x2 = max(x2, pb.GetRight()); y2 = max(y2, pb.GetBottom())
    return pcbnew.ToMM((x1+x2)/2.0), pcbnew.ToMM((y1+y2)/2.0)

def set_pose(ref, x, y, rot):
    """place so the footprint's pad bbox is centred on (x, y)."""
    fp = fps[ref]
    fp.SetOrientationDegrees(0)
    fp.SetOrientationDegrees(rot)
    fp.SetPosition(V(x, y))
    cx, cy = pad_center(fp)
    fp.SetPosition(V(x + (x - cx), y + (y - cy)))

HALO = 0.6   # mm added around footprint bbox (body + graphics)
def extent(ref):
    """full footprint bbox (incl. body/courtyard, excl. text) + HALO, in mm."""
    fp = fps[ref]
    tomm = pcbnew.ToMM
    try:
        bb = fp.GetBoundingBox(False, False)   # no text, no invisible text
    except Exception:
        bb = fp.GetBoundingBox(False)
    return [tomm(bb.GetLeft())-HALO, tomm(bb.GetTop())-HALO,
            tomm(bb.GetRight())+HALO, tomm(bb.GetBottom())+HALO]

def overlap(a, c):
    return not (a[2] <= c[0] or c[2] <= a[0] or a[3] <= c[1] or c[3] <= a[1])

# ---- anchors ----
front = set()
back = set()
# TFT header on front, left side of screen zone (centred coords now)
set_pose("J3", 12, 30, 0); front.add("J3")
# ESP32 two rows, on back, directly behind the TFT zone
set_pose("J1", 32, 30, 0); back.add("J1")
set_pose("J2", 52, 30, 0); back.add("J2")
# 5 nav buttons, + shape, front, lower half
for ref, (x, y) in {"SW1":(34,78),"SW5":(34,92),"SW2":(34,106),
                    "SW3":(20,92),"SW4":(48,92)}.items():
    set_pose(ref, x, y, 0); front.add(ref)

occupied = [extent(r) for r in list(front) + list(back)]

# ---- parts to auto-pack onto the back ----
rest = [r for r in fps if r not in front and r not in back and not r.startswith("#")]
# place chunky modules first so they get prime spots
def area(r):
    e = extent(r); return (e[2]-e[0])*(e[3]-e[1])
rest.sort(key=area, reverse=True)

# candidate centres: upper zone first (smaller y), fill toward bottom
cands = []
y = MARGIN
while y <= H - MARGIN:
    x = MARGIN
    while x <= W - MARGIN:
        cands.append((x, y)); x += 2.0
    y += 2.0

placed, overflow = [], []
ox = W + 15
for r in rest:
    done = False
    for (cx, cy) in cands:
        set_pose(r, cx, cy, 0)
        e = extent(r)
        if e[0] < MARGIN or e[1] < MARGIN or e[2] > W-MARGIN or e[3] > H-MARGIN:
            continue
        if any(overlap(e, o) for o in occupied):
            continue
        occupied.append(e); placed.append((r, cx, cy)); back.add(r); done = True
        break
    if not done:
        set_pose(r, ox, 20, 0); overflow.append(r); back.add(r); ox += 15

# ---- self-check: brute-force pairwise overlap of final extents ----
allrefs = list(front) + list(back)
ext = {r: extent(r) for r in allrefs}
bad = 0
for i in range(len(allrefs)):
    for j in range(i+1, len(allrefs)):
        ra, rb = allrefs[i], allrefs[j]
        if overlap(ext[ra], ext[rb]):
            print("  OVERLAP:", ra, ext[ra], "<->", rb, ext[rb]); bad += 1
print("self-check overlapping pairs:", bad)

# ---- flip the back parts to B.Cu (mirror about pad centre so they stay put) ----
for r in back:
    fp = fps[r]
    if not fp.IsFlipped():
        cx, cy = pad_center(fp)
        fp.Flip(V(cx, cy), False)

# ---- board outline on Edge.Cuts ----
# remove any existing edge drawings first
for d in list(b.GetDrawings()):
    if d.GetLayer() == pcbnew.Edge_Cuts:
        b.Remove(d)
s = pcbnew.PCB_SHAPE(b)
s.SetShape(pcbnew.SHAPE_T_RECTANGLE)
s.SetStart(V(0, 0)); s.SetEnd(V(W, H))
s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.15))
b.Add(s)

pcbnew.SaveBoard(PCB, b)

# ---- report ----
def under_tft(x, y):  # TFT screen zone ~ x6..54, y6..66
    return 6 <= x <= 54 and 6 <= y <= 66
print("\nplaced on back:", len(placed))
nu = sum(1 for (r,x,y) in placed if under_tft(x,y))
print("  under the TFT zone:", nu, " elsewhere on back:", len(placed)-nu)
if overflow:
    print("DID NOT FIT (parked off-board to the right):", overflow)
else:
    print("everything fit on the board.")
