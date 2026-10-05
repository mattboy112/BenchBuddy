import math
import os
import sys

import cadquery as cq

# Board (ESP32_S3 GPIO Extension Board). Listings say 80x80 or 82x82 mm; measure yours.
BOARD_W = 80.0          # left-right
BOARD_D = 80.0          # front-back
BOARD_T = 1.6
HOLE_DX = 76.5          # mounting hole spacing, left-right
HOLE_DY = 76.0          # mounting hole spacing, front-back
JACK_X = 28.0           # DC jack center, measured from the board center toward the right

# Tray
POCKET = 82.8           # inside size; fits an 80 or 82 mm board, the screws locate it
POCKET_R = 1.0
WALL = 2.4
FLOOR = 3.4
STANDOFF_H = 4.5        # gap under the board for header pin stubs
STANDOFF_D = 5.6
STANDOFF_FLARE_D = 8.0
PILOT_D = 2.6           # M3 screws thread straight into PETG
PILOT_DEPTH = 7.1
LIP = 1.2               # wall height above the board's top surface
CORNER_R = 4.0
JACK_NOTCH_W = 16.0

# Dovetail joint between tray and stand
DOVE_X = 30.0
DOVE_BASE = 6.0
DOVE_TOP = 8.0
DOVE_H = 1.5
DOVE_CLEAR = 0.3

# Tilt stand
TILT = 15.0
STAND_FRONT_H = 4.0
STOP_H = 6.0
STOP_T = 4.0
RAIL_W = 6.0
BAR_W = 80.0
BAR_D = 6.0
REAR_BAR_H = 8.0

OUT = POCKET + 2 * WALL
PCB_Z = FLOOR + STANDOFF_H
TRAY_H = PCB_Z + BOARD_T + LIP
HOLES = [(sx * HOLE_DX / 2, sy * HOLE_DY / 2) for sx in (-1, 1) for sy in (-1, 1)]

T = math.radians(TILT)
Y0 = STOP_T * math.cos(T) + STOP_H * math.sin(T)
H0 = STAND_FRONT_H
RAIL_L = OUT


def slope_pt(s, n):
    return (Y0 + s * math.cos(T) - n * math.sin(T), H0 + s * math.sin(T) + n * math.cos(T))


def standoff(height):
    r_top, r_base, flare = STANDOFF_D / 2, STANDOFF_FLARE_D / 2, 1.5
    prof = [(0, 0), (r_base, 0), (r_top, flare), (r_top, height), (0, height)]
    return cq.Workplane("XZ").polyline(prof).close().revolve(360, (0, 0, 0), (0, 1, 0))


def dove_groove_cutter(length):
    c = DOVE_CLEAR
    k = (DOVE_TOP - DOVE_BASE) / 2 / DOVE_H
    b, h, mz = DOVE_BASE / 2 + c, DOVE_H + c, 0.4
    top = b + k * h
    prof = [(-b - 0.4, -0.01), (b + 0.4, -0.01), (b + k * mz, mz), (top, h), (-top, h), (-b - k * mz, mz)]
    return cq.Workplane("XZ").polyline(prof).close().extrude(-length / 2, both=True)


def dove_tongue(length):
    b, t, h = DOVE_BASE / 2, DOVE_TOP / 2, DOVE_H
    prof = [(-b, -0.01), (b, -0.01), (t, h), (-t, h)]
    return cq.Workplane("XZ").polyline(prof).close().extrude(-length)


def make_tray():
    body = (cq.Workplane("XY").rect(OUT, OUT).extrude(TRAY_H)
            .edges("|Z").fillet(CORNER_R)
            .faces(">Z").edges().fillet(0.8)
            .faces("<Z").edges().chamfer(0.5))
    pocket = (cq.Workplane("XY").workplane(offset=FLOOR).rect(POCKET, POCKET).extrude(TRAY_H)
              .edges("|Z").fillet(POCKET_R))
    body = body.cut(pocket)
    for x, y in HOLES:
        body = body.union(standoff(STANDOFF_H + 0.01).translate((x, y, FLOOR - 0.01)))
    for x, y in HOLES:
        pilot = (cq.Workplane("XY").workplane(offset=PCB_Z - PILOT_DEPTH).center(x, y)
                 .circle(PILOT_D / 2).extrude(PILOT_DEPTH + 1))
        lead = (cq.Workplane("XY").workplane(offset=PCB_Z - 0.5).center(x, y)
                .circle(PILOT_D / 2).workplane(offset=0.51).circle(PILOT_D / 2 + 0.5).loft())
        body = body.cut(pilot).cut(lead)
    notch = (cq.Workplane("XY").workplane(offset=PCB_Z)
             .center(JACK_X, -(POCKET / 2 + WALL / 2))
             .rect(JACK_NOTCH_W, WALL + 2).extrude(TRAY_H)
             .edges("|Y").edges("<Z").fillet(1.5))
    body = body.cut(notch)
    for x in (-DOVE_X, DOVE_X):
        body = body.cut(dove_groove_cutter(OUT + 2).translate((x, 0, 0)))
    return body


def rail_profile(with_stop):
    pts = []
    if with_stop:
        a = slope_pt(-STOP_T, STOP_H)
        pts += [(a[0], 0), a, slope_pt(0, STOP_H), slope_pt(0, 0)]
    else:
        p0 = slope_pt(0, 0)
        pts += [(p0[0], 0), p0]
    pe = slope_pt(RAIL_L, 0)
    pts += [pe, (pe[0], 0)]
    return pts


def make_stand():
    stand = None
    for x, stop in ((-DOVE_X, True), (0.0, False), (DOVE_X, True)):
        rail = (cq.Workplane("YZ").polyline(rail_profile(stop)).close().extrude(RAIL_W)
                .translate((x - RAIL_W / 2, 0, 0)))
        stand = rail if stand is None else stand.union(rail)
    y_front = slope_pt(-STOP_T, STOP_H)[0]
    y_back = slope_pt(RAIL_L, 0)[0]
    front_bar = (cq.Workplane("XY").center(0, (y_front + Y0 + BAR_D) / 2)
                 .rect(BAR_W, Y0 + BAR_D - y_front).extrude(H0 - 0.6))
    rear_bar = (cq.Workplane("XY").center(0, y_back - BAR_D / 2)
                .rect(BAR_W, BAR_D).extrude(REAR_BAR_H))
    stand = stand.union(front_bar).union(rear_bar)
    try:
        stand = stand.edges("|Z").fillet(1.0)
    except Exception:
        pass
    for x in (-DOVE_X, DOVE_X):
        tongue = (dove_tongue(RAIL_L).translate((x, 0, 0))
                  .rotate((0, 0, 0), (1, 0, 0), TILT).translate((0, Y0, H0)))
        stand = stand.union(tongue)
    return stand


def place_tray_on_stand(shape):
    return shape.translate((0, OUT / 2, 0)).rotate((0, 0, 0), (1, 0, 0), TILT).translate((0, Y0, H0))


def make_board_mock():
    z0 = PCB_Z
    top = z0 + BOARD_T
    P = 2.54
    groups = {"pcb": [], "hdr_black": [], "hdr_red": [], "hdr_yellow": [], "pins": [], "sockets": [],
              "devboard": [], "module": [], "usb": [], "jack": [], "parts": []}
    pcb = (cq.Workplane("XY").workplane(offset=z0).rect(BOARD_W, BOARD_D).extrude(BOARD_T)
           .edges("|Z").fillet(1.5))
    for x, y in HOLES:
        pcb = pcb.cut(cq.Workplane("XY").workplane(offset=z0 - 1).center(x, y).circle(1.6).extrude(5))
    groups["pcb"].append(pcb)

    def column(cx, cy, rows, color, h=2.5):
        d = rows * P
        groups[color].append(cq.Workplane("XY").workplane(offset=top).center(cx, cy).rect(P, d).extrude(h))
        pts = [(cx, cy - d / 2 + P * (j + 0.5)) for j in range(rows)]
        groups["pins"].append(cq.Workplane("XY").workplane(offset=top + h).pushPoints(pts).rect(0.64, 0.64).extrude(6.0))
        groups["pins"].append(cq.Workplane("XY").workplane(offset=z0 - 3.0).pushPoints(pts).rect(0.64, 0.64).extrude(3.0))

    for cx, c in ((-34.4, "hdr_black"), (-31.6, "hdr_red"), (-28.8, "hdr_yellow")):
        column(cx, 11.5, 17, c)
    for cx, c in ((28.8, "hdr_yellow"), (31.6, "hdr_red"), (34.4, "hdr_black")):
        column(cx, 9.0, 19, c)
    for cy in (-16.9, -29.6):
        column(-32.5, cy, 4, "hdr_black")
        column(-29.9, cy, 4, "hdr_red")
    column(-20.8, 8.0, 22, "hdr_yellow")
    column(20.6, 8.0, 22, "hdr_yellow")
    sock_h = 8.5
    for cx in (-12.7, 12.7, -16.5, 16.5):
        groups["sockets"].append(cq.Workplane("XY").workplane(offset=top).center(cx, 9.3).rect(P, 22 * P).extrude(sock_h))
    dz = top + sock_h + 1.0
    groups["devboard"].append(cq.Workplane("XY").workplane(offset=dz).center(0, 6.0).rect(28.0, 69.0).extrude(1.6)
                              .edges("|Z").fillet(1.0))
    groups["module"].append(cq.Workplane("XY").workplane(offset=dz + 1.6).center(0, 31.5).rect(18.0, 25.5).extrude(3.2))
    for cx in (-5.5, 5.5):
        groups["usb"].append(cq.Workplane("XY").workplane(offset=dz + 1.6).center(cx, -24.9).rect(8.9, 7.3).extrude(3.2)
                             .edges("|Y").fillet(1.2))
    groups["jack"].append(cq.Workplane("XY").workplane(offset=top).center(JACK_X, -34.0).rect(9.0, 14.0).extrude(11.0))
    for cx, w, d, h in ((10.0, 6.6, 6.1, 2.3), (2.0, 6.6, 6.6, 7.7), (18.5, 6.6, 6.6, 7.7)):
        groups["parts"].append(cq.Workplane("XY").workplane(offset=top).center(cx, -33.0).rect(w, d).extrude(h))
    out = {}
    for k, lst in groups.items():
        u = lst[0]
        for o in lst[1:]:
            u = u.union(o)
        out[k] = u
    return out


def make_fit_test():
    t = 1.2
    frame = (cq.Workplane("XY").rect(OUT, OUT).extrude(t).edges("|Z").fillet(CORNER_R)
             .cut(cq.Workplane("XY").rect(OUT - 14, OUT - 14).extrude(t).edges("|Z").fillet(2.0)))
    for a in (45, -45):
        frame = frame.union(cq.Workplane("XY").rect(OUT * 1.3, 6).extrude(t).rotate((0, 0, 0), (0, 0, 1), a)
                            .intersect(cq.Workplane("XY").rect(OUT - 1, OUT - 1).extrude(t)))
    for x, y in HOLES:
        frame = frame.union(cq.Workplane("XY").center(x, y).circle(5.5).extrude(t))
        frame = frame.union(standoff(3.0).translate((x, y, t - 0.01)))
        frame = frame.cut(cq.Workplane("XY").center(x, y).circle(PILOT_D / 2).extrude(t + 4))
    front = (cq.Workplane("XY").center(0, -(POCKET / 2 + WALL / 2)).rect(OUT - 2 * CORNER_R, WALL)
             .extrude(t + 3.0 + BOARD_T + LIP))
    notch = (cq.Workplane("XY").workplane(offset=t + 3.0).center(JACK_X, -(POCKET / 2 + WALL / 2))
             .rect(JACK_NOTCH_W, WALL + 2).extrude(20))
    frame = frame.union(front.cut(notch))
    return frame


def make_dove_coupons():
    L = 25.0
    tongue_block = (cq.Workplane("XY").rect(14, L).extrude(4.0)
                    .union(dove_tongue(L).translate((0, -L / 2, 4.0))))
    groove_block = (cq.Workplane("XY").rect(16, L).extrude(FLOOR)
                    .cut(dove_groove_cutter(L + 2)))
    return tongue_block.translate((-12, 0, 0)), groove_block.translate((12, 0, 0))


def write_plates(out_dir, bed=245.0):
    try:
        import trimesh
    except ImportError:
        print("trimesh not installed, skipping 3MF plates (pip install trimesh)")
        return

    def load(name):
        return trimesh.load(os.path.join(out_dir, name), force="mesh")

    def place(m, cx, cy):
        m = m.copy()
        b = m.bounds
        m.apply_translation([cx - (b[0][0] + b[1][0]) / 2, cy - (b[0][1] + b[1][1]) / 2, -b[0][2]])
        return m

    plates = {
        "BenchBuddy_Dock_plate_main.3mf": [("tray", "BenchBuddy_Dock_tray.stl", -50), ("tilt stand", "BenchBuddy_Dock_tilt_stand.stl", 50)],
        "BenchBuddy_Dock_plate_fit_test.3mf": [("fit test", "BenchBuddy_Dock_fit_test.stl", -25), ("dovetail test", "BenchBuddy_Dock_dovetail_test.stl", 52)],
    }
    for fname, items in plates.items():
        scene = trimesh.Scene()
        for label, stl_name, dx in items:
            scene.add_geometry(place(load(stl_name), bed / 2 + dx, bed / 2), node_name=label, geom_name=label)
        scene.export(os.path.join(out_dir, fname))


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    tray = make_tray()
    stand = make_stand()
    fit = make_fit_test()
    tongue_c, groove_c = make_dove_coupons()
    board = make_board_mock()

    def stl(wp, name, tol=0.02, ang=0.15):
        cq.exporters.export(wp, os.path.join(out_dir, name), tolerance=tol, angularTolerance=ang)

    stl(tray, "BenchBuddy_Dock_tray.stl")
    stl(stand, "BenchBuddy_Dock_tilt_stand.stl")
    stl(fit, "BenchBuddy_Dock_fit_test.stl")
    stl(tongue_c.union(groove_c), "BenchBuddy_Dock_dovetail_test.stl")
    cq.exporters.export(tray, os.path.join(out_dir, "BenchBuddy_Dock_tray.step"))
    cq.exporters.export(stand, os.path.join(out_dir, "BenchBuddy_Dock_tilt_stand.step"))

    asm = cq.Assembly(name="BenchBuddy_Dock")
    asm.add(stand, name="tilt_stand", color=cq.Color(0.16, 0.18, 0.21))
    asm.add(place_tray_on_stand(tray), name="tray", color=cq.Color(0.25, 0.80, 0.88))
    board_colors = {"pcb": (0.05, 0.05, 0.06), "hdr_black": (0.1, 0.1, 0.1), "hdr_red": (0.85, 0.16, 0.14),
                    "hdr_yellow": (0.95, 0.8, 0.15), "pins": (0.85, 0.7, 0.3), "sockets": (0.08, 0.08, 0.08),
                    "devboard": (0.06, 0.06, 0.08), "module": (0.75, 0.76, 0.78), "usb": (0.8, 0.8, 0.82),
                    "jack": (0.1, 0.1, 0.1), "parts": (0.3, 0.3, 0.32)}
    for k, wp in board.items():
        asm.add(place_tray_on_stand(wp), name="board_" + k, color=cq.Color(*board_colors[k]))
    asm.save(os.path.join(out_dir, "BenchBuddy_Dock_assembly.step"))

    write_plates(out_dir)

    viewer_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "viewer")
    stl_dir = os.path.join(viewer_dir, "stl")
    os.makedirs(stl_dir, exist_ok=True)
    cq.exporters.export(stand, os.path.join(stl_dir, "stand.stl"), tolerance=0.05, angularTolerance=0.2)
    cq.exporters.export(place_tray_on_stand(tray), os.path.join(stl_dir, "tray.stl"), tolerance=0.05, angularTolerance=0.2)
    for k, wp in board.items():
        cq.exporters.export(place_tray_on_stand(wp), os.path.join(stl_dir, "board_" + k + ".stl"),
                            tolerance=0.05, angularTolerance=0.2)
    builder = os.path.join(viewer_dir, "build_viewer.py")
    if os.path.exists(builder):
        import runpy
        runpy.run_path(builder, run_name="__main__")

    return {"tray": tray, "stand": stand, "fit": fit, "board": board,
            "dims": {"OUT": OUT, "TRAY_H": TRAY_H, "PCB_Z": PCB_Z, "Y0": Y0,
                     "stand_back_y": slope_pt(RAIL_L, 0)[0], "stand_back_h": slope_pt(RAIL_L, 0)[1]}}


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "exports")
    r = main(out)
    print({k: round(v, 2) for k, v in r["dims"].items()})
