# Preparing a rig

Everything in this page runs **outside Unreal**, once per rig or whenever the
rig changes. It needs usdRig's environment: its OpenUSD 26.08 build with
OpenExec, its Python, and `rigExecBake`. Build usdRig first with
`..\usdRig\bin\build_rigexec.bat`.

A character needs three files:

| File | Made by | Read by |
| --- | --- | --- |
| `<Name>.rigexec` | `rigExecBake --poseable` | `URigExecComponent` (the binary runtime) |
| `<Name>_controls.json` | `Scripts/export_controls.py`, then `build_control_rig.py` adds the element names | `build_control_rig.py`, `URigExecComponent` |
| `<Name>_picker.json` | `Scripts/export_picker.py` | The RigExec Picker tab (optional) |

It also needs the **source stage** itself, which is not copied. The
component reads mesh topology and materials from it.

## The biped

```bat
prepare_biped.bat
```

This rebuilds all three files for the example project from
`..\usdRig\examples\biped\Biped_stack.usda` into
`Examples\RigExecBiped\RigExecData`. It sources usdRig's `bin\_env.bat`, so
`RIG`, `USD`, `PY`, `PYTHONPATH` and `PXR_PLUGINPATH_NAME` are usdRig's own.
Set `RIGEXEC_ROOT` if usdRig is not at `..\usdRig`.

Afterwards, rebuild the project's Control Rig, level and sequence. The
control set may have changed. See
[editor-workflow.md](editor-workflow.md#rebuilding-the-example-project).

## Any rig, step by step

From a shell where usdRig's environment is set up
(`call ..\usdRig\bin\_env.bat`):

```bat
rigExecBake <stage.usda> --rig <rig prim> --frames 1 --poseable -o <Name>.rigexec
"%PY%" Scripts\export_controls.py <stage.usda> <Name>_controls.json [<rig prim>]
"%PY%" Scripts\export_picker.py   <stage.usda> <Name>_picker.json
```

Keep the three files together. The picker is found by name from the
controls file (`X_controls.json` → `X_picker.json`).

### `rigExecBake --poseable`

- `--poseable` stores the rig's **property chains** as programs. Without
  it, chains replay their recorded values, so a face slider that drives only
  chains (and every intermediate avar) does nothing in Unreal. The load log
  says `property chains live` or `property chains replayed`.
- `--frames 1` is enough. Unreal poses from avars, not from baked frames. The
  component selects the first baked frame as the base pose.
- `--rig` picks the rig prim when the stage has more than one.

### `export_controls.py`

```
python export_controls.py <stage.usda> <out.json> [<rig prim path>]
```

This walks the rig's controls and writes, for each one:

- whether the animator handles it (a picker button or touch region names
  it). The rest are the rig's helpers and become Control Rig **nulls**
- its parent control and its rest offset relative to that parent
- the avar channels it carries and how they compose: `avars:rotationOrder`,
  `avars:rotationSign`, `avars:unitScaleFactor`
- its guide shape and colour, so the Control Rig gizmos look like
  usdview's
- every other animatable avar on it (double/float/int/bool), such as
  `ikfk`, `space` or face sliders, with its default, slider range, hard
  clamp and labels
- every IK/FK limb, with its joints, FK controls, IK control, effector, pole
  and rest offsets, so the picker can match a switch

It imports usdRig's usdview plug-in models (`gizmoMath`, `avarEditorModel`,
`ikfkMatch`) from `<usdRig>/plugin/rigExecUsdview`. The usdRig root is
`RIGEXEC_ROOT`, else `../usdRig` next to this repository. The math matches
usdview's gizmos and Avar Editor exactly.

### `export_picker.py`

```
python export_picker.py <stage.usda> <out.json>
```

This reads the stage's picker prims the way usdview's picker does
(`pickerScene.load_all`). Each button's outline is the path usdview paints
(`pickerUI._path_for`), flattened and triangulated so that Slate can draw it
without a path renderer. It needs the Qt that usdview uses
(`pxr.Usdviewq.qt`) and registers the RigExec schema plugin first.

## File formats

### The controls file

```jsonc
{
  "stage": "Biped_stack.usda",           // informational
  "rigPath": "/Biped/Rig",
  "upAxis": "Y",
  "metersPerUnit": 0.01,
  "assetToWorld": [16 numbers],          // asset placement, USD row-vector, row-major
  "controls": [
    {
      "path": "/Biped/Rig/.../L_Arm",    // rig prim path (what SetAvar / GetControlFrame take)
      "name": "L_Arm",
      "animator": true,                  // false: a rig helper
      "kind": "null",                    // present for helpers: drawn as a Control Rig null
      "element": "L_Arm",                // Control Rig element name (written by build_control_rig.py)
      "parent": 12,                      // index into controls, -1 for a root
      "offset": [16 numbers],            // rest offset relative to the parent, asset space
      "value":  [16 numbers],            // rest value
      "rotationOrder": "XYZ",            // one of XYZ XZY YXZ YZX ZXY ZYX
      "rotationSign": [1, 1, 1],
      "unitScale": 1.0,
      "translate": [true, true, true],   // which of avars:tx..sz exist
      "rotate":    [true, true, true],
      "scale":     [true, true, true],
      "defaults": [tx, ty, tz, rx, ry, rz, sx, sy, sz],
      "guide": { "shape": "circle", "matrix": [16 numbers], "color": [r, g, b] },
      "fade": { "source": "/.../L_Arm.avars:ikfk", "invert": false }   // or null
    }
  ],
  "channels": [
    {
      "control": 95,                     // index into controls
      "attr": "avars:space",
      "type": "double",
      "default": 0.0,
      "range": [0.0, 2.0],               // slider range (authored limits, else 0..1)
      "clamp": [0.0, 2.0],               // hard limits, if any
      "labels": ["world", "hip_swivel", "hips"],   // enum-like channels
      "element": "space"                 // written by build_control_rig.py
    }
  ],
  "limbs": [
    {
      "switch": "/.../L_Arm.avars:ikfk",
      "ikValue": 1.0,                    // the switch value meaning IK
      "joints": [root, middle, end],
      "fkControls": [3 paths],
      "ikControl": "...", "effector": "...", "pole": "...",
      "fkOffsets": [[16], [16], [16]],   // control · joint⁻¹ per FK control, at rest
      "effectorOffset": [16],            // end joint · effector⁻¹, at rest
      "poleDistance": 42.0
    }
  ]
}
```

All matrices are USD's convention: row vectors, 16 numbers row-major, asset
space, in stage units. The plugin converts them to Unreal (see
[architecture.md](architecture.md#coordinates-and-units)).

### The picker file

```jsonc
{
  "pickers": [
    {
      "name": "Uman",
      "rig": "/Biped/Rig",
      "panels": [
        {
          "label": "Body", "size": [w, h], "fill": [r, g, b, a],
          "buttons": [
            {
              "id": "...",
              "box": [x, y, width, height],          // unrotated hit box, panel units, y down
              "vertices": [x, y, ...],               // the outline, triangulated:
              "triangles": [i, j, k, ...],           //   indices into vertices
              "outlines": [[x, y, ...], ...],        // stroke polylines
              "fill": [r, g, b, a], "stroke": [...], "strokeWidth": 1.0,
              "textColor": [...], "valueColor": [...],
              "text": "IK", "fontSize": 8, "bold": false, "align": "left" | "center" | "right",
              "decoration": false,                   // drawn, never clickable
              "targets": ["/.../L_Arm", ...],        // rig controls it selects
              "attribute": {                         // an attribute (channel) button
                "path": "/.../L_Arm.avars:ikfk", "labels": ["FK", "IK"], "invert": false
              },
              "dial": "/.../L_Arm.avars:ikfk",       // with "mode": drawn only while the dial
              "mode": "ik" | "fk",                   //   channel is in that mode
              "command": "zero_ctrls" | "ctrl_vis",  // an action button
              "mirror": "<id of the mirrored button>"
            }
          ]
        }
      ]
    }
  ]
}
```

The exact field set follows usdRig's picker schema. Treat
`export_picker.py` and `SRigExecPicker.cpp` as the source of truth.

### The `.rigexec` file

This is usdRig's binary rig image. Its sections (steps, poses, weights,
geometry, input table, cones, clusters, property chains, external movers)
are documented in usdRig (`libs/rigExecRuntime/README.md` and
`docs/concepts/baked-vs-dynamic.md`). Unreal treats it as opaque bytes and
hands it to `RigExecRuntimeReader::Open`.

## What the runtime can and cannot pose

- **Live:** transform avars on controls (`avars:tx` … `avars:sz`), and any
  input a property chain reads when the bake is `--poseable` (face sliders,
  `ikfk`, `space`, and so on).
- **Refused, so dropped at load:** a property chain's own output, and
  anything the bake folded into a constant. The component logs
  `N of M channels are not live in <file>: …`.
- **Plugin movers** (revision opcode 16) need a kernel installed with
  `SetExternalKernel`. The plugin installs none, so those movers pass their
  points through, and the runtime names them in every `Execute`'s
  diagnostics.
