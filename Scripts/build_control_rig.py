"""
Builds a Control Rig asset from export_controls.py's description.

Runs inside the Unreal Editor's Python:

    build_control_rig.py <controls.json> <asset path, e.g. /Game/RigExec/CR_Biped>

Every animator control becomes a Euler-transform control, and every other
avar on it an animation channel; the rig's helper transforms become nulls,
so the hierarchy is the rig's own without handing out what the rig drives.

The rig is built in USD space: nothing is converted per control. Every
control and null keeps its USD rest offset and value as authored (row
vectors, centimetres), and one root null, "UsdSpace", above the rig's top
controls carries the whole asset from USD's Y-up right-handed space into
Unreal's Z-up left-handed one -- the Y/Z swap, a mirror. Its controls are
then USD's own, value for value, so animation always matches the rig.

The element names are written back into the JSON as "element", with
"space": "usd" and the root's name as "root", which RigExecComponent reads.
"""

import json
import sys

import unreal


def _Multiply(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
            for r in range(4) for c in range(4)]


# The one transform a USD-space rig converts with: Y/Z swapped (row vector).
_SWAP = [1, 0, 0, 0,
         0, 0, 1, 0,
         0, 1, 0, 0,
         0, 0, 0, 1]
USD_SPACE_ROOT = "UsdSpace"


def AsTransform(m, scale):
    """A USD row-vector matrix (16 numbers) as it is, translation in
    centimetres: the USD-space rig's own offsets and values."""
    rows = [unreal.Plane(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], 0.0) for r in range(3)]
    rows.append(unreal.Plane(m[12] * scale, m[13] * scale, m[14] * scale, 1.0))
    return unreal.MathLibrary.conv_matrix_to_transform(unreal.Matrix(*rows))


# The rig's unit guide shapes (radius or half-extent 1) in Unreal's shape
# library. Planar ones lie flat in XY there, which is where a USD XZ shape
# lands after the Y/Z swap.
_SHAPES = {
    "circle": "Circle_Thick",
    "box": "Square_Thick",
    "cube": "Box_Thin",
    "sphere": "Sphere_Thin",
    "diamond": "Diamond_Thick",
    "pyramid": "Pyramid_Thick",
}


def UnitFactors(blueprint):
    """Per shape, the scale per mesh axis that turns the library's drawn
    shape (its mesh times its library transform) into the unit shape. A
    flat shape's thin axis takes its width's factor."""
    factors = {}
    libraries = list(blueprint.get_editor_property("shape_libraries"))
    for entry in reversed(libraries):
        library = entry if hasattr(entry, "get_editor_property") else unreal.load_asset(str(entry))
        if library is None:
            continue
        for shape in library.get_editor_property("shapes"):
            name = str(shape.get_editor_property("shape_name"))
            if name not in _SHAPES.values() or name in factors:
                continue
            box = shape.get_editor_property("shape_proxy").get_bounding_box()
            scale = shape.get_editor_property("transform").scale3d
            ext = [max(abs(box.min.x), abs(box.max.x)) * scale.x,
                   max(abs(box.min.y), abs(box.max.y)) * scale.y,
                   max(abs(box.min.z), abs(box.max.z)) * scale.z]
            width = max(ext[0], ext[1])
            factors[name] = tuple(1.0 / (e if e > 0.1 * width else width) for e in ext)
    return factors


def ShapeTransform(guide, scale, factors):
    """The control's guide as a Control Rig shape transform: the library
    shape sized to the unit shape, then the guide matrix in Unreal's space
    (USD's own, in a USD-space rig)."""
    name = _SHAPES.get(guide["shape"], _SHAPES["circle"])
    unit = factors.get(name, (1.0, 1.0, 1.0))
    m = guide["matrix"]
    # No per-control swap: the library's XY plane is turned onto the guide's
    # USD XZ plane by a quarter turn about X (library Y -> USD Z, library Z
    # -> -USD Y), then the guide matrix as authored.
    turn = [[1, 0, 0], [0, 0, 1], [0, -1, 0]]
    rows = []
    for r in range(3):
        rows.append(unreal.Plane(*[unit[r] * scale * sum(turn[r][k] * m[k * 4 + c] for k in range(3))
                                   for c in range(3)], 0.0))
    rows.append(unreal.Plane(m[12] * scale, m[13] * scale, m[14] * scale, 1.0))
    return name, unreal.MathLibrary.conv_matrix_to_transform(unreal.Matrix(*rows))


def _Color(name):
    if name.startswith("L_"):
        return unreal.LinearColor(0.1, 0.4, 1.0, 1.0)
    if name.startswith("R_"):
        return unreal.LinearColor(1.0, 0.15, 0.1, 1.0)
    return unreal.LinearColor(1.0, 0.85, 0.1, 1.0)


def Build(jsonPath, assetPath):
    with open(jsonPath) as f:
        description = json.load(f)
    # A control's transforms are taken as authored; the one conversion is
    # the root's.
    convert = AsTransform
    if description.get("upAxis", "Y") != "Y":
        raise RuntimeError("only Y-up stages are converted")
    scale = description.get("metersPerUnit", 0.01) * 100.0
    assetToWorld = description["assetToWorld"]

    if unreal.EditorAssetLibrary.does_asset_exist(assetPath):
        unreal.EditorAssetLibrary.delete_asset(assetPath)
    blueprint = unreal.ControlRigBlueprintFactory.create_new_control_rig_asset(assetPath)
    hierarchy = blueprint.get_hierarchy()
    factors = UnitFactors(blueprint)
    unreal.log("RigExec: shape unit factors %s" % factors)
    controller = blueprint.get_hierarchy_controller()

    swap = unreal.MathLibrary.conv_matrix_to_transform(unreal.Matrix(
        *[unreal.Plane(*_SWAP[r * 4:r * 4 + 4]) for r in range(4)]))
    root = controller.add_null(USD_SPACE_ROOT, unreal.RigElementKey(), swap,
                               transform_in_global=False, setup_undo=False)
    hierarchy.set_local_transform(root, swap, True, False)
    unreal.log("RigExec: USD-space root %s = %s" % (root.name, swap))

    keys = []
    for control in description["controls"]:
        parent = control["parent"]
        offset = control["offset"]
        if parent < 0:
            offset = _Multiply(offset, assetToWorld)
        parentKey = keys[parent] if parent >= 0 else root
        if not control.get("animator", True):
            # The posed rest frame, local to the parent.
            rest = convert(_Multiply(control["value"], offset), scale)
            key = controller.add_null(control["name"], parentKey, rest,
                                      transform_in_global=False, setup_undo=False)
            control["element"] = str(key.name)
            control["kind"] = "null"
            keys.append(key)
            continue
        settings = unreal.RigControlSettings()
        settings.control_type = unreal.RigControlType.EULER_TRANSFORM
        settings.animation_type = unreal.RigControlAnimationType.ANIMATION_CONTROL
        settings.display_name = control["name"]
        guide = control.get("guide")
        shapeName, shapeTransform = ShapeTransform(guide, scale, factors) if guide else (
            "Circle_Thick", unreal.Transform(scale=[0.15, 0.15, 0.15]))
        settings.shape_name = shapeName
        settings.shape_color = (unreal.LinearColor(*guide["color"], 1.0)
                                if guide and "color" in guide else _Color(control["name"]))
        initial = convert(control["value"], scale)
        value = unreal.RigHierarchy.make_control_value_from_euler_transform(
            unreal.EulerTransform(initial.translation, initial.rotation.rotator(),
                                  initial.scale3d))
        key = controller.add_control(control["name"], parentKey, settings, value,
                                     setup_undo=False)
        hierarchy.set_control_offset_transform(key, convert(offset, scale),
                                               initial=True, affect_children=False)
        hierarchy.set_control_shape_transform(key, shapeTransform, initial=True)
        control["element"] = str(key.name)
        control["kind"] = "control"
        keys.append(key)

    for channel in description["channels"]:
        settings = unreal.RigControlSettings()
        settings.control_type = unreal.RigControlType.FLOAT
        settings.animation_type = unreal.RigControlAnimationType.ANIMATION_CHANNEL
        label = channel["attr"].split(":", 1)[1]
        settings.display_name = label
        labels = channel.get("labels")
        if labels:
            # A space index, 0 to the last space; between two blends them.
            settings.display_name = "%s - %s" % (label, " / ".join(labels))
        # The slider spans the channel's minimum to maximum (Anim Details
        # uses a channel's raw range as its slider range), so this is the
        # usdview Avar Editor's range for the same attribute; it clamps only
        # where the rig's hard limits are that range.
        low, high = channel.get("range", [0.0, 1.0])
        clamp = channel.get("clamp")
        settings.limit_enabled = [unreal.RigControlLimitEnabled(clamp is not None, clamp is not None)]
        settings.minimum_value = unreal.RigHierarchy.make_control_value_from_float(low)
        settings.maximum_value = unreal.RigHierarchy.make_control_value_from_float(high)
        name = label.replace(":", "_")
        key = controller.add_animation_channel(name, keys[channel["control"]], settings,
                                               setup_undo=False)
        hierarchy.set_control_value(
            key, unreal.RigHierarchy.make_control_value_from_float(channel["default"]),
            unreal.RigControlValueType.INITIAL)
        hierarchy.set_control_value(
            key, unreal.RigHierarchy.make_control_value_from_float(channel["default"]),
            unreal.RigControlValueType.CURRENT)
        channel["element"] = str(key.name)

    blueprint.recompile_vm()
    # A full compile, so the generated class carries the blueprint's shape
    # libraries; without it the viewport finds no shapes to draw.
    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    unreal.EditorAssetLibrary.save_loaded_asset(blueprint, False)
    description["space"] = "usd"
    description["root"] = str(root.name)
    with open(jsonPath, "w") as f:
        json.dump(description, f, indent=1)
    animated = sum(1 for c in description["controls"] if c["kind"] == "control")
    unreal.log("RigExec: %s has %d controls, %d nulls and %d channels"
               % (assetPath, animated, len(keys) - animated, len(description["channels"])))
    return blueprint


if __name__ == "__main__":
    Build(sys.argv[1], sys.argv[2])
