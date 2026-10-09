"""
Describes a stage's control pickers for the Unreal picker panel.

Runs under the Python that owns the rig's USD build (OpenUSD 26.08 with
OpenExec, and the Qt usdview uses), never inside Unreal:

    python export_picker.py <stage.usda> <out.json>

The pickers are read exactly as the usdview picker reads them
(pickerScene.load_all), and each button's outline is the path the usdview
picker paints (pickerUI._path_for), flattened to polygons and triangulated
here so the Unreal panel draws the same shapes without a path renderer.

Coordinates are panel units with y down, relative to the corner of the
panel's content box (pickerModel.content_box). Each button keeps its
unrotated box, which is what a click is tested against.
"""

import json
import os
import sys

from pxr import Usd
import rigexec

# The picker prims are found by schema type, so the schema is registered
# before pickerScene resolves its types.
rigexec.load_schema_plugin()
# The usdRig checkout: RIGEXEC_ROOT, else the sibling ../usdRig.
_ROOT = os.environ.get("RIGEXEC_ROOT") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, "usdRig")
_USDVIEW = os.path.join(_ROOT, "plugin", "rigExecUsdview")
sys.path.insert(0, os.path.abspath(_USDVIEW))
from pxr.Usdviewq.qt import QtGui  # noqa: E402

import pickerModel  # noqa: E402
import pickerScene  # noqa: E402
import pickerUI  # noqa: E402


def _Area(points):
    return 0.5 * sum(points[i][0] * points[(i + 1) % len(points)][1] -
                     points[(i + 1) % len(points)][0] * points[i][1]
                     for i in range(len(points)))


def _Clean(points):
    """A polygon without its closing repeat or coincident neighbours."""
    out = []
    for p in points:
        if not out or abs(p[0] - out[-1][0]) + abs(p[1] - out[-1][1]) > 1e-6:
            out.append(p)
    while len(out) > 1 and (abs(out[0][0] - out[-1][0]) +
                            abs(out[0][1] - out[-1][1])) <= 1e-6:
        out.pop()
    return out


def _Inside(p, a, b, c):
    def side(u, v, w):
        return (v[0] - u[0]) * (w[1] - u[1]) - (v[1] - u[1]) * (w[0] - u[0])
    return side(a, b, p) >= 0 and side(b, c, p) >= 0 and side(c, a, p) >= 0


def _Triangulate(points):
    """Ear clipping. Indices into `points`, three per triangle; a fan for
    whatever is left if the outline is not simple."""
    n = len(points)
    if n < 3:
        return []
    order = list(range(n))
    if _Area(points) < 0:
        order.reverse()
    tris = []
    guard = 0
    while len(order) > 3 and guard < n * n:
        guard += 1
        clipped = False
        for k in range(len(order)):
            i0, i1, i2 = (order[k - 1], order[k], order[(k + 1) % len(order)])
            a, b, c = points[i0], points[i1], points[i2]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 0:
                continue
            if any(_Inside(points[j], a, b, c) for j in order
                   if j not in (i0, i1, i2)):
                continue
            tris += [i0, i1, i2]
            del order[k]
            clipped = True
            break
        if not clipped:
            break
    for k in range(1, len(order) - 1):
        tris += [order[0], order[k], order[k + 1]]
    return tris


def _Transform(button):
    """Button-local to panel space, as the usdview picker paints it."""
    t = QtGui.QTransform()
    t.translate(button.x, button.y)
    if button.rotation:
        t.translate(button.w * 0.5, button.h * 0.5)
        t.rotate(button.rotation)
        t.translate(-button.w * 0.5, -button.h * 0.5)
    return t


def _Geometry(button, ox, oy):
    path = _Transform(button).map(pickerUI._path_for(button))
    vertices, triangles, outlines = [], [], []
    for polygon in path.toFillPolygons():
        points = _Clean([(p.x() - ox, p.y() - oy) for p in polygon])
        base = len(vertices)
        vertices += points
        triangles += [base + i for i in _Triangulate(points)]
    for polygon in path.toSubpathPolygons():
        points = _Clean([(p.x() - ox, p.y() - oy) for p in polygon])
        if len(points) > 1:
            outlines.append([round(c, 3) for p in points for c in p])
    return ([round(c, 3) for p in vertices for c in p], triangles, outlines)


def _Button(button, ox, oy):
    vertices, triangles, outlines = _Geometry(button, ox, oy)
    record = {
        "id": button.id,
        "box": [round(button.x - ox, 3), round(button.y - oy, 3),
                round(button.w, 3), round(button.h, 3)],
        "vertices": vertices,
        "triangles": triangles,
        "outlines": outlines,
        "fill": list(button.fill),
        "stroke": list(button.stroke),
        "strokeWidth": button.stroke_width,
        "text": button.text,
        "fontSize": button.font_size,
        "bold": button.bold,
        "align": button.h_align,
        "textColor": list(button.text_color),
        "valueColor": list(button.value_color),
        "decoration": button.decoration,
        "overlay": button.shape in ("widgetControl", "slider"),
        "targets": button.targets,
        "mirror": (button.mirror or "").rsplit("/", 1)[-1],
    }
    if button.attr_target:
        target = button.attr_target
        record["attribute"] = {
            "path": "%s.%s" % (target["path"], target["attr"]),
            "labels": list(target.get("enum") or []),
            "invert": bool(target.get("invert")),
        }
    if button.command in pickerModel.IMPLEMENTED_COMMANDS:
        record["command"] = button.command
    if button.mode and button.dial:
        record["mode"] = button.mode
        record["dial"] = button.dial
    return record


def Export(stagePath):
    stage = Usd.Stage.Open(stagePath)
    if not stage:
        raise RuntimeError("could not open %s" % stagePath)
    pickers = []
    for picker in pickerScene.load_all(stage):
        panels = []
        for panel in picker.panels:
            (ox, oy), (w, h) = pickerModel.content_box(picker, panel)
            # visible() with no modes: every button the rig has, in
            # painter order; the panel filters the IK/FK halves itself.
            buttons = [_Button(b, ox, oy)
                       for b in picker.visible(panel.id, modes=None)]
            panels.append({"label": panel.label, "size": [w, h],
                           "fill": list(panel.fill), "buttons": buttons})
        pickers.append({"name": picker.name, "rig": picker.rig_path,
                        "panels": panels})
    return {"pickers": pickers}


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    document = Export(argv[1])
    with open(argv[2], "w", newline="") as out:
        json.dump(document, out, separators=(",", ":"))
    count = sum(len(p["buttons"]) for k in document["pickers"]
                for p in k["panels"])
    print("%d pickers, %d buttons" % (len(document["pickers"]), count))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
