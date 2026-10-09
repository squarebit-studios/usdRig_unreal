"""
Describes a rig's animator controls for the Unreal Control Rig front end.

Runs under the Python that owns the rig's USD build (OpenUSD 26.08 with
OpenExec), never inside Unreal:

    python export_controls.py <stage.usda> <out.json> [<rig prim path>]

Each control is written with whether the animator handles it (a picker
button or touch region names it; the rest are the rig's own helpers),
its parent control, its rest offset relative
to that parent (asset space, USD row-vector convention, 16 numbers
row-major), the avar channels it carries and how they compose
(rotation order, rotation sign, unit scale). build_control_rig.py turns the
file into a Control Rig asset; RigExecComponent reads it to turn control
values back into avars.
"""

import json
import os
import struct
import sys

from pxr import Gf, Sdf, Usd, UsdGeom
import rigexec

# The usdview plug-in's gizmo, avar and IK/FK models live in the usdRig
# checkout: RIGEXEC_ROOT, else the sibling ../usdRig.
_ROOT = os.environ.get("RIGEXEC_ROOT") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, "usdRig")
sys.path.insert(0, os.path.abspath(os.path.join(_ROOT, "plugin", "rigExecUsdview")))
import gizmoMath  # noqa: E402
import avarEditorModel  # noqa: E402
import ikfkMatch  # noqa: E402

_TRANSFORM = gizmoMath.AVAR_T + gizmoMath.AVAR_R + gizmoMath.AVAR_S
# Control avars that configure the control rather than pose it.
_NON_CHANNELS = {gizmoMath.AVAR_UNIT_SCALE, gizmoMath.AVAR_RSPIN}
_CHANNEL_TYPES = {Sdf.ValueTypeNames.Double, Sdf.ValueTypeNames.Float,
                  Sdf.ValueTypeNames.Int, Sdf.ValueTypeNames.Bool}


def _Matrix(m):
    return [m[i][j] for i in range(4) for j in range(4)]


def _Value(prim, name, fallback):
    attr = prim.GetAttribute(name)
    value = attr.Get() if attr else None
    return float(value) if value is not None else fallback


def _Guide(prim):
    """How the viewport draws the control: a unit shape (radius or
    half-extent 1; circle and box flat in XZ) turned to guide:planeNormal,
    scaled by guide:scaleX/Y/Z in the control's own axes, turned by
    guide:orient, then moved by guide:offset -- as one matrix in the control's frame, and its colour."""
    def get(name, fallback):
        attr = prim.GetAttribute(name)
        value = attr.Get() if attr else None
        return fallback if value is None else value

    shape = str(get("guide:shape", "circle"))
    normal = str(get("guide:planeNormal", "Y"))
    plane = Gf.Matrix4d(1.0)
    if shape in ("circle", "box"):
        if normal == "X":
            plane.SetRotate(Gf.Rotation(Gf.Vec3d(0, 0, 1), -90.0))
        elif normal == "Z":
            plane.SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), 90.0))
    scale = Gf.Matrix4d(1.0).SetScale(Gf.Vec3d(
        float(get("guide:scaleX", 1.0)), float(get("guide:scaleY", 1.0)),
        float(get("guide:scaleZ", 1.0))))
    orient = Gf.Matrix4d(1.0).SetRotate(
        Gf.Rotation(Gf.Quatd(get("guide:orient", Gf.Quatf(1.0)))))
    offset = Gf.Matrix4d(1.0).SetTranslate(Gf.Vec3d(get("guide:offset", Gf.Vec3d(0.0))))
    color = get("guide:displayColor", None)
    guide = {"shape": shape, "matrix": _Matrix(plane * scale * orient * offset)}
    if color is not None:
        if hasattr(color, "__len__") and len(color) and hasattr(color[0], "__len__"):
            color = color[0]
        guide["color"] = [float(color[0]), float(color[1]), float(color[2])]
    return guide


def _Fade(prim):
    """What fades the control in the viewport: the attribute
    guide:displayOpacity is connected to (a limb's IK/FK dial), and
    whether the fade is inverted. None when nothing drives it."""
    attr = prim.GetAttribute("guide:displayOpacity")
    sources = attr.GetConnections() if attr else []
    if not sources:
        return None
    invert = prim.GetAttribute("guide:displayOpacityInvert")
    return {"source": str(sources[0]),
            "invert": bool(invert.Get()) if invert else False}


def _Range(attr, value):
    """(slider range, clamp) for a channel. The slider range is the one the
    usdview Avar Editor gives the same attribute (avarEditorModel): its
    authored soft limits, else its hard ones, else 0-1 for a float and 0-10
    for an int, widened to hold the default. The clamp is the hard range,
    and only where it is also the slider range -- a channel whose hard
    limits reach past its soft ones may go past the slider, as in usdview."""
    limits = attr.GetCustomDataByKey("limits")
    ranges = {}
    if isinstance(limits, dict):
        for which in ("soft", "hard"):
            entry = limits.get(which)
            try:
                low, high = float(entry["minimum"]), float(entry["maximum"])
            except (KeyError, TypeError, ValueError):
                continue
            if high > low:
                ranges[which] = [low, high]
    slider = avarEditorModel.AuthoredLimits(attr)
    if slider is None:
        slider = (0.0, 10.0) if attr.GetTypeName() == Sdf.ValueTypeNames.Int             else (0.0, 1.0)
    slider = [min(slider[0], value), max(slider[1], value)]
    hard = ranges.get("hard")
    clamp = hard if hard is not None and hard == slider else None
    return slider, clamp


def _Limbs(stage, rigPath):
    """Each IK/FK limb, with the offsets ikfkMatch measures at its rest
    pose, so a host can match one half onto the other without the stage."""
    evaluate = ikfkMatch.Evaluator(stage, rigPath)
    limbs = []
    for limb in ikfkMatch.FindLimbs(stage):
        if limb.rigRoot != rigPath:
            continue
        rest = ikfkMatch.MeasureRest(limb, stage, evaluate,
                                     Usd.TimeCode.Default())
        limbs.append({
            "switch": str(limb.switchPath),
            "ikValue": limb.ikValue,
            "joints": [str(j) for j in limb.joints],
            "fkControls": [str(c) for c in limb.fkControls],
            "ikControl": str(limb.ikControl),
            "effector": str(limb.effector),
            "pole": str(limb.pole) if limb.pole is not None else "",
            # control * joint^-1 per FK control; end joint * effector^-1.
            "fkOffsets": [_Matrix(m) for m in rest.fk],
            "effectorOffset": _Matrix(rest.effector),
            "poleDistance": rest.poleDistance or 0.0,
        })
    return limbs


def Export(stagePath, rigPath=None):
    stage = Usd.Stage.Open(stagePath)
    if not stage:
        raise RuntimeError("could not open %s" % stagePath)
    if rigPath is None:
        roots = [p.GetPath() for p in stage.Traverse()
                 if p.GetTypeName() == "RigExecRoot"]
        if not roots:
            raise RuntimeError("no RigExecRoot in %s" % stagePath)
        rigPath = roots[0]
    rigPath = Sdf.Path(str(rigPath))
    rig = rigexec.Rig(stage, str(rigPath))
    rig.compile()
    rest = rig.evaluate(-1)
    framed = set(rest.control_paths())

    written = set()
    for prim in stage.Traverse():
        rel = prim.GetRelationship("rigExec:moves")
        if rel:
            written.update(rel.GetTargets())

    # A space switch's active index reads a control avar: name its spaces.
    spaceLabels = {}
    for prim in stage.Traverse():
        if prim.GetTypeName() != "RigExecSpaceSwitch":
            continue
        rel = prim.GetRelationship("rigExec:activeSpaceAttribute")
        labels = prim.GetAttribute("rigExec:spaceLabels")
        if rel and labels and labels.Get():
            for target in rel.GetTargets():
                spaceLabels[target] = [str(t) for t in labels.Get()]

    # The animator's controls are the ones the rig's own interface hands
    # out: picker buttons and touch regions.
    animator = set()
    for prim in stage.Traverse():
        for relName in ("rigExec:picker:controls", "rigExec:touch:control",
                        "touchpose:control"):
            rel = prim.GetRelationship(relName)
            if rel:
                animator.update(t.GetPrimPath() for t in rel.GetTargets())

    controls, channels, posed, names = [], [], [], set()
    indexOf = {}
    for prim in Usd.PrimRange(stage.GetPrimAtPath(rigPath)):
        if prim.GetTypeName() != "RigExecControl" or \
                str(prim.GetPath()) not in framed:
            continue
        path = prim.GetPath()
        parent = path.GetParentPath()
        while parent != Sdf.Path.absoluteRootPath and parent not in indexOf:
            parent = parent.GetParentPath()
        parentIndex = indexOf.get(parent, -1)
        raw = rest.control_matrices_bytes([str(path)])
        frame = Gf.Matrix4d(*[struct.unpack_from("<4d", raw, 32 * r)
                              for r in range(4)])

        order = str(prim.GetAttribute(gizmoMath.AVAR_ORDER).Get() or "XYZ")
        sign = gizmoMath.RotationSign(prim)
        units = _Value(prim, gizmoMath.AVAR_UNIT_SCALE, 1.0)
        scaled = gizmoMath.ReadsScaleAvars(prim)
        has = {name: bool(prim.GetAttribute(name)) and
               prim.GetAttribute(name).GetPath() not in written
               for name in _TRANSFORM}
        defaults = [_Value(prim, n, 1.0 if n in gizmoMath.AVAR_S else 0.0)
                    for n in _TRANSFORM]
        avars = gizmoMath.ComposeAvarMatrix(
            defaults[0] * units, defaults[1] * units, defaults[2] * units,
            defaults[6] if scaled else 1.0, defaults[7] if scaled else 1.0,
            defaults[8] if scaled else 1.0,
            defaults[3] * sign[0], defaults[4] * sign[1],
            defaults[5] * sign[2], 0.0, order)
        # The frame the avars are expressed in, carried by the parent
        # control's posed frame: offset * parentPosed * ... = P.
        offset = avars.GetInverse() * frame
        if parentIndex >= 0:
            offset = offset * posed[parentIndex].GetInverse()

        name = prim.GetName()
        while name in names:
            name += "_"
        names.add(name)
        index = len(controls)
        indexOf[path] = index
        posed.append(frame)
        controls.append({
            "path": str(path),
            "name": name,
            "animator": path in animator,
            "guide": _Guide(prim),
            "fade": _Fade(prim),
            "parent": parentIndex,
            "offset": _Matrix(offset),
            "value": _Matrix(avars),
            "rotationOrder": order,
            "rotationSign": list(sign),
            "unitScale": units,
            "translate": [has[n] for n in gizmoMath.AVAR_T],
            "rotate": [has[n] for n in gizmoMath.AVAR_R],
            "scale": [has[n] and scaled for n in gizmoMath.AVAR_S],
            "defaults": defaults,
        })
        if path not in animator:
            continue
        for attr in prim.GetAttributes():
            attrName = attr.GetName()
            if (not attrName.startswith("avars:") or attrName in _TRANSFORM
                    or attrName in _NON_CHANNELS
                    or attr.GetTypeName() not in _CHANNEL_TYPES
                    or attr.GetPath() in written):
                continue
            value = attr.Get()
            channel = {
                "control": index,
                "attr": attrName,
                "type": str(attr.GetTypeName()),
                "default": float(value) if value is not None else 0.0,
            }
            if attr.GetPath() in spaceLabels:
                channel["labels"] = spaceLabels[attr.GetPath()]
            channel["range"], clamp = _Range(attr, channel["default"])
            if clamp is not None:
                channel["clamp"] = clamp
            channels.append(channel)

    assetRoot = stage.GetPrimAtPath(rigPath.GetParentPath())
    assetToWorld = UsdGeom.Xformable(assetRoot).ComputeLocalToWorldTransform(
        Usd.TimeCode.Default()) if assetRoot.IsA(UsdGeom.Xformable) \
        else Gf.Matrix4d(1.0)
    return {
        # The file name only: a record of where the rig came from, not a
        # path to resolve, and no machine's layout belongs in it.
        "stage": os.path.basename(stage.GetRootLayer().realPath),
        "rigPath": str(rigPath),
        "upAxis": str(UsdGeom.GetStageUpAxis(stage)),
        "metersPerUnit": UsdGeom.GetStageMetersPerUnit(stage),
        "assetToWorld": _Matrix(assetToWorld),
        "controls": controls,
        "channels": channels,
        "limbs": _Limbs(stage, rigPath),
    }


def main(argv):
    if len(argv) not in (3, 4):
        sys.stderr.write(__doc__)
        return 2
    description = Export(argv[1], argv[3] if len(argv) == 4 else None)
    with open(argv[2], "w") as out:
        json.dump(description, out, indent=1)
    print("%d controls, %d channels, %d limbs" % (
        len(description["controls"]), len(description["channels"]),
        len(description["limbs"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
