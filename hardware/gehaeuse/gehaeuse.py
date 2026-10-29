# Gehaeuse fuer das ESP32 Sound Mini S3 (Sandwich: MAX98357A Ruecken an Ruecken unter dem ESP32-S3-Zero)
# Parametrisch mit CadQuery. Aufruf: python gehaeuse.py  -> STL/STEP im Ordner "export"
#
# Masse aus dem 3D-Modell docs/3d-modell/Aufbau3D_Mini-S3.html (Martins Fotos vom 27.10.2026).
# Modell-Koordinaten (wie im 3D-Modell): x entlang ESP (USB-Ende = minus), z quer (Reihe A = minus),
# y = Hoehe, ESP-Platine y 0..1, Bauteilseite des ESP bei y < 0, MAX-Seite bei y > 0.
# Im Gehaeuse liegt die ESP-Seite OBEN (BOOT-Taste und LED unter dem Deckel):
#   Gehaeuse X = x, Y = z, Z = Z_OFF - y   (Drehung um 180 Grad, keine Spiegelung)
import os, sys
import cadquery as cq

# ---------------- Parameter (alles in mm) ----------------
WAND    = 1.6     # Wandstaerke
BODEN   = 1.6     # Bodenstaerke
DECKEL  = 1.6     # Deckelstaerke
LIPPE   = 2.0     # Hoehe der Klemmlippe am Deckel
TOL     = 0.15    # Spiel Lippe <-> Innenwand (bei strammem Sitz auf 0,2 erhoehen)

# Innenraum in Modell-Koordinaten
X_MIN, X_MAX = -12.5, 14.3   # USB-C ragt bis -12,3; am anderen Ende Platz fuer die Kabel
Z_MIN, Z_MAX = -9.6, 14.0    # Reihe A bis -9; JST-PH Lautsprecher endet bei 13,9
Y_TOP  = -5.0                # Deckel-Unterseite: USB-Stecker (6,5 mm hoch) muss passen
Y_BOT  = 10.0                # Boden-Innenseite: Stifte Port B enden bei 9,5
Z_OFF  = Y_BOT + BODEN       # Gehaeuse-Z = Z_OFF - y

# Lage der Teile (Modell)
P = 2.54
def px(k): return -10.16 + P * k   # Pinraster, k = 0 am USB-Ende
ROW_A, ROW_B = -7.62, 7.62
USB_Z  = (-6.4, 6.4)               # USB-C-Stecker 12,5 breit, Mitte z = 0
USB_Y  = (-4.95, 1.75)             # Buchse y -3,2..0, Stecker-Umspritzung 6,5 hoch
SPK_X  = (-6.2, 1.0)               # JST-PH 2-pol am MAX, Stecker 6 breit
SPK_Y  = (4.7, 9.7)
BOOT   = (-3.2, -5.0)              # (x, z) Taster BOOT
RESET  = (-3.2, 5.0)
LED    = (-3.2, 0.0)
BOOT_TOP = -2.05                   # Oberkante Tasterkappe (y)

def box(x0, y0, z0, x1, y1, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))

def mbox(x0, x1, ya, yb, za, zb):
    """Quader in Modell-Koordinaten (x, y, z) -> Gehaeuse."""
    return box(x0, min(za, zb), Z_OFF - max(ya, yb), x1, max(za, zb), Z_OFF - min(ya, yb))

def rbox(x0, y0, z0, x1, y1, z1, r):
    return (cq.Workplane("XY").workplane(offset=z0).center((x0 + x1) / 2, (y0 + y1) / 2)
            .rect(x1 - x0, y1 - y0).extrude(z1 - z0).edges("|Z").fillet(r))

def cyl(x, y, z0, z1, r):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(r).extrude(z1 - z0)

# Gehaeuse-Koordinaten
IX0, IX1, IY0, IY1 = X_MIN, X_MAX, Z_MIN, Z_MAX
OX0, OX1, OY0, OY1 = IX0 - WAND, IX1 + WAND, IY0 - WAND, IY1 + WAND
H_BASE = Z_OFF - Y_TOP             # Hoehe Unterteil
ECKE_R = 2.5
def gz(y): return Z_OFF - y

def unterteil():
    b = rbox(OX0, OY0, 0, OX1, OY1, H_BASE, ECKE_R)
    b = b.cut(box(IX0, IY0, BODEN, IX1, IY1, H_BASE + 1))
    # Stuetzen: unter dem MAX (freie Stelle ohne Bauteile) und unter dem ESP-Ende hinter dem MAX
    b = b.union(mbox(2.6, 5.8, 5.1, Y_BOT + 0.1, -5.0, 5.0))
    b = b.union(mbox(7.0, 11.2, 1.0, Y_BOT + 0.1, -5.4, 5.4))
    # USB-C: Kerbe in der Stirnwand, oben offen (der Deckel schliesst sie)
    b = b.cut(box(OX0 - 1, USB_Z[0], gz(USB_Y[1]), IX0 + 0.5, USB_Z[1], H_BASE + 1))
    # Lautsprecher-Stecker JST-PH durch die Seitenwand (Seite Reihe B)
    b = b.cut(box(SPK_X[0], IY1 - 0.5, gz(SPK_Y[1]), SPK_X[1], OY1 + 1, gz(SPK_Y[0])))
    # Kabelschlitz Port A (4 Adern) + optional Port C, oben offen
    b = b.cut(box(IX1 - 0.5, -6.0, gz(-2.0), OX1 + 1, 6.0, H_BASE + 1))
    # Bodenfenster unter den 2-pol. Stiften: Dupont-Stecker von unten aufstecken (Port B, IO5/IO6)
    for z in (ROW_A, ROW_B):
        b = b.cut(box(px(7) - 1.6, z - 1.5, -1, px(8) + 1.6, z + 1.5, BODEN + 1))
    # Rastnuten fuer die Deckelnasen (lange Seiten)
    for y, s in ((IY0, -1), (IY1, 1)):
        b = b.cut(cq.Workplane("YZ").workplane(offset=-6.0).center(y, H_BASE - LIPPE / 2 - 0.1)
                  .circle(0.5).extrude(12.0))
    # Befestigungslaschen an den Stirnseiten (Schraube M2 oder Kabelbinder)
    for x, s in ((OX0, -1), (OX1, 1)):
        t = rbox(x - 6 if s < 0 else x - 1, -4.0, 0, x + 1 if s < 0 else x + 6, 4.0, 1.8, 1.5)
        b = b.union(t).cut(cyl(x + s * 3.3, 0, -1, 4, 1.2))
    # Beschriftung unten (0,5 mm vertieft, gespiegelt lesbar von unten)
    def txt(s, x, y, size):
        return (cq.Workplane("XY").workplane(offset=0.5).center(x, y)
                .transformed(rotate=(180, 0, 0)).text(s, size, 1.0, halign="center", valign="center"))
    for s, x, y, sz in [("B", (px(7) + px(8)) / 2 - 5.2, ROW_B, 2.6),
                        ("5/6", (px(7) + px(8)) / 2 - 6.2, ROW_A, 2.2),
                        ("SOUND MINI S3", -3.0, 0.0, 2.4)]:
        b = b.cut(txt(s, x, y, sz))
    return b

def deckel():
    d = rbox(OX0, OY0, 0, OX1, OY1, DECKEL, ECKE_R)
    # Klemmlippe, an USB-Kerbe und Kabelschlitz ausgespart
    lip = box(IX0 + TOL, IY0 + TOL, -LIPPE, IX1 - TOL, IY1 - TOL, 0.01)
    lip = lip.cut(box(IX0 + TOL + 1.0, IY0 + TOL + 1.0, -LIPPE - 1, IX1 - TOL - 1.0, IY1 - TOL - 1.0, 1))
    lip = lip.cut(box(IX0 - 1, USB_Z[0] - 0.3, -LIPPE - 1, IX0 + 2, USB_Z[1] + 0.3, 1))
    lip = lip.cut(box(IX1 - 2, -6.3, -LIPPE - 1, IX1 + 1, 6.3, 1))
    # Rastnasen
    for y in (IY0 + TOL, IY1 - TOL):
        lip = lip.union(cq.Workplane("YZ").workplane(offset=-5.0).center(y, -LIPPE / 2 - 0.1)
                        .circle(0.35).extrude(10.0))
    d = d.union(lip)
    # Niederhalter auf das ESP-Ende (zwischen Antenne und Loetpunkten)
    for z in (-5.4, 5.4):
        d = d.union(box(9.8, z - 0.8, -(0.0 - Y_TOP - 0.15), 11.4, z + 0.8, 0.01))
    # BOOT-Taste: federnde Zunge mit Stoessel (Taste = WLAN-Start)
    bx, bz = BOOT
    zx0, zx1, zw = bx - 2.6, bx + 8.0, 2.2       # Zunge: freies Ende ueber BOOT, Wurzel Richtung +x
    slot = box(zx0 - 0.6, bz - zw - 0.6, -1, zx1, bz + zw + 0.6, DECKEL + 1)
    slot = slot.cut(box(zx0, bz - zw, -2, zx1 + 1, bz + zw, DECKEL + 2))
    d = d.cut(slot)
    d = d.cut(box(zx0, bz - zw, 1.0, zx1, bz + zw, DECKEL + 1))       # Zunge auf 1,0 mm duenner
    d = d.union(cyl(bx, bz, -(BOOT_TOP - Y_TOP) + 0.4, 0.01, 1.2))  # Stoessel, 0,4 mm Luft
    d = d.union(cyl(bx, bz, 0.99, 1.6, 1.8))                        # Druckpunkt oben
    # Fenster fuer die RGB-LED und Loch fuer RESET (mit Buroklammer)
    d = d.cut(cyl(LED[0], LED[1], -LIPPE - 1, DECKEL + 1, 1.3))
    d = d.cut(cyl(RESET[0], RESET[1], -LIPPE - 1, DECKEL + 1, 0.8))
    # Beschriftung (0,5 mm vertieft)
    def txt(s, x, y, size, rot=0):
        return (cq.Workplane("XY").workplane(offset=DECKEL - 0.5).center(x, y)
                .transformed(rotate=(0, 0, rot)).text(s, size, 1.0, halign="center", valign="center"))
    for s, x, y, sz, r in [("WLAN", bx + 3.2, bz - 4.3, 1.8, 0), ("LED", LED[0] + 4.8, LED[1], 1.8, 0),
                           ("RST", RESET[0] + 4.2, RESET[1], 1.8, 0), ("USB", IX0 + 1.6, 0, 2.0, 90),
                           ("SPK", (SPK_X[0] + SPK_X[1]) / 2, IY1 - 1.8, 1.8, 0),
                           ("A", IX1 - 1.6, 0, 2.2, 90)]:
        d = d.cut(txt(s, x, y, sz, r))
    return d

def baugruppe():
    """Vereinfachtes Sandwich fuer Vorschau und Kollisionskontrolle (Modell-Koordinaten wie im 3D-Modell)."""
    t = {}
    t["esp"] = mbox(-11.75, 11.75, 0, 1, -9, 9)
    t["usb"] = mbox(-12.3, -4.9, -3.2, 0, -4.5, 4.5)
    t["chip"] = mbox(0.9, 5.9, -0.9, 0, -1.9, 3.1)
    t["boot"] = mbox(-4.6, -1.8, -2.05, 0, -6.8, -3.2)
    t["reset"] = mbox(-4.6, -1.8, -2.05, 0, 3.2, 6.8)
    t["led"] = mbox(-4.0, -2.4, -0.6, 0, -0.8, 0.8)
    t["ant"] = mbox(9.3, 11.5, -1.0, 0, -3.8, 1.4)
    t["hdr"] = mbox(px(0) - 1.27, px(6) + 1.27, 1.0, 3.5, ROW_A - 1.25, ROW_A + 1.25)
    for z in (ROW_A, ROW_B):
        t["hdr2%d" % z] = mbox(px(7) - 1.27, px(8) + 1.27, 1.0, 3.5, z - 1.25, z + 1.25)
        for x in (px(7), px(8)):
            t["pin%d%d" % (x, z)] = mbox(x - 0.32, x + 0.32, -0.9, 9.5, z - 0.32, z + 0.32)
    for k in range(7):
        if k != 2:
            t["pa%d" % k] = mbox(px(k) - 0.32, px(k) + 0.32, -1.0, 6.0, ROW_A - 0.32, ROW_A + 0.32)
    t["max"] = mbox(-11.6, 6.4, 3.5, 5.1, -9.0, 10.4)
    t["maxic"] = mbox(-2.7, 0.3, 5.1, 5.9, -3.9, -0.9)
    t["jst"] = mbox(-5.6, 0.4, 5.1, 9.1, 7.7, 13.9)
    r = None
    for v in t.values():
        r = v if r is None else r.union(v)
    return r

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "export"
    os.makedirs(out, exist_ok=True)
    u, d = unterteil(), deckel()
    print("Aussenmasse: %.1f x %.1f x %.1f mm (mit Deckel, ohne Laschen)" % (OX1 - OX0, OY1 - OY0, H_BASE + DECKEL))
    cq.exporters.export(u, f"{out}/Gehaeuse_Unterteil.stl", tolerance=0.02, angularTolerance=0.1)
    cq.exporters.export(u, f"{out}/Gehaeuse_Unterteil.step")
    d_druck = d.rotate((0, 0, 0), (1, 0, 0), 180)   # Deckel mit der Oberseite aufs Druckbett
    cq.exporters.export(d_druck, f"{out}/Gehaeuse_Deckel.stl", tolerance=0.02, angularTolerance=0.1)
    cq.exporters.export(d, f"{out}/Gehaeuse_Deckel.step")
    if "--pruefen" in sys.argv:
        bg = baugruppe()
        dd = d.translate((0, 0, H_BASE))
        print("Kollision Baugruppe/Unterteil: %.3f mm3, Baugruppe/Deckel: %.3f mm3"
              % (bg.intersect(u).val().Volume(), bg.intersect(dd).val().Volume()))
