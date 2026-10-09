# Architecture

How the RigExec plugin turns a baked usdRig character into something you can
pose and key in Unreal.

## The short version

usdRig evaluates rigs with OpenExec inside USD. Unreal has no OpenExec, and
its USD build is not the one usdRig compiles against. The plugin therefore
does not evaluate the USD rig. It evaluates a **baked program**:

1. `rigExecBake --poseable` compiles the rig into a `.rigexec` file. This is a
   binary image of the rig's step tables, poses, weights, geometry revisions
   and (with `--poseable`) its property chains as programs.
2. `RigExecRuntimeReader` (from usdRig's `libs/rigExecRuntime`) replays that
   program in plain C++17, with no USD. It takes new avar values with
   `SetAvar`, re-runs with `Execute`, and hands back deformed points and
   control frames.
3. The plugin feeds Control Rig values in as avars and draws the points as
   `UDynamicMeshComponent`s.

The runtime is held to the USD evaluator point for point by usdRig's own tests
(`testRigExecRuntimeLiveFace` and the binary round-trip tests). As a result,
what you see in Unreal is what usdview shows for the same avars.

## Modules

```
Plugins/RigExec/
├── RigExec.uplugin                 depends on USDCore and ControlRig; Win64 only
└── Source/
    ├── RigExecRuntime/             Type=Runtime, LoadingPhase=Default
    │   ├── Public/RigExecActor.h            ARigExecActor
    │   ├── Public/RigExecComponent.h        URigExecComponent
    │   └── Public/RigExecRuntimeLibrary.h   URigExecRuntimeLibrary (BP/Python helpers)
    ├── RigExecEditor/              Type=Editor, LoadingPhase=PostEngineInit
    │   ├── RigExecEditorModule.cpp          tab, Tools menu entries and console commands
    │   ├── RigExecPickerModel.*             the picker's buttons, clicks and painting, shared
    │   ├── SRigExecPicker.*                 the docked picker panel (Slate)
    │   ├── RigExecHoverLayout.*             the hover picker's layout and gestures (no Slate)
    │   ├── RigExecHoverPicker.*             the hover picker: viewport overlay and input
    │   ├── RigExecMarkingMenuModel.*        the marking menu's gesture, layout and menus (no Slate)
    │   ├── RigExecMarkingMenu.*             the marking menu: right-click routing, painting, commands
    │   ├── RigExecDraw.h                    disc, ring and line helpers the overlays share
    │   ├── RigExecTouchPose.*               Touch Pose
    │   └── RigExecComponentDetails.*        the Rig component's render-path banner and toast
    └── ThirdParty/RigExecLib/      Type=External
        ├── include/                         rigExecRuntime/, rigExecBinary/, rigExecMath/ headers
        ├── lib/Win64/                       rigExecRuntime.lib, rigExecBinary.lib
        └── SOURCE.txt                       the usdRig commit the copy came from
```

**RigExecRuntime** depends publicly on `Core`, `CoreUObject`, `Engine`,
`UnrealUSDWrapper`, `ControlRig` and `RigVM`. It depends privately on
`GeometryCore`, `GeometryFramework`, `USDUtilities`, `USDClasses`, `Json` and
`RigExecLib`. In editor builds it also depends on `UnrealEd` and
`ControlRigEditor`. It turns on RTTI and calls
`UnrealUSDWrapper.CheckAndSetupUsdSdk` so that it can include `pxr/` headers
the way Unreal's own USD modules do. It turns on exceptions because the
runtime headers use the standard library freely.

**RigExecEditor** holds the pickers, Touch Pose and the Rig component's
details customization. It depends on the runtime module for `ARigExecActor`
and `URigExecComponent`, and on `LevelEditor` for the level viewports the
hover picker draws over.

**RigExecLib** is an External module: headers plus prebuilt static libraries.
The runtime headers are plain C++, so the plugin includes them between
`THIRD_PARTY_INCLUDES_START`/`END`. It also pushes and pops Unreal's `check`
and `verify` macros around them.

## Drawing on the GPU

By default each mesh is drawn by a `URigExecSkinComponent`, RigExec's GPU skin:

- **Build, once per load.** It takes the Dynamic Mesh's topology, UVs, colours and material slots. Render
  vertices are split per (point, UV element, polygon), which gives 142,922 on the biped. UVs stay seams, and
  each polygon owns its colour, so Touch Pose borders are crisp. For each render vertex it also builds the list
  of triangles around its point.
- **Per pose.** The posed positions are uploaded once into a dynamic buffer and copied into the local vertex
  factory's position buffer. A compute shader (`RigExecShaders` module, `Shaders/Private/RigExecNormals.usf`,
  one thread per render vertex) writes 16-bit packed tangents, which are then copied into the factory's tangent
  buffer:
  - the normal is the area-weighted sum of face normals around the point, so it is smooth across UV seams;
  - the tangent is the UV gradient over the vertex's own island, orthogonalized, with handedness in W.

  The CPU does no normal work and no full mesh re-upload.
- **Colours.** The Touch Pose tint is uploaded only when it changes.
- **The Dynamic Meshes stay as the data.** Picking, Touch Pose and a subdivision surface read them, and their
  visibility says whether anything else draws them. They no longer draw themselves: main pass, depth, shadows
  and ray tracing are off. Their edits are made silently (`bDeferChangeEvents`) while a skin draws them, and
  with change events while they are hidden (a subdivision surface listening). A skin follows its mesh's
  visibility, so Subdivs taking over hides it, and handing back shows it, caught up.

Per pose on the biped, the mesh side takes about 5 ms on the GPU path against about 29 ms on the CPU fallback.
The rig evaluation itself (16–22 ms in the runtime) is now the larger part.

**CPU fallback.** Where the GPU path cannot run (below Shader Model 5, or the shader not compiled for the
platform), or with **Draw On GPU** off, the Dynamic Meshes draw themselves as before: CPU normals and a
re-upload each pose. The component says so in **Render Path**, the editor shows a toast and a banner at the
top of the Rig component's Details, and the log records it.

**Motion vectors.** The skin writes velocity for TSR and motion blur the same way Squarebit Subdivs does,
with no custom shader code:

- **Its own vertex factory.** `FRigExecSkinVertexFactory`, in the `RigExecShaders` module because vertex
  factory types must exist before shaders compile, is the engine's local factory plus the GPU-skin pass-through
  velocity path from `LocalVertexFactory.ush`.
- **Two position buffers.** Each pose, the last pose is GPU-copied into a previous-positions buffer before the
  new one is uploaded. The factory's pass-through loose parameters point at both, stamped with the game frame
  (`GFrameCounter`).
- **No motion for a still pose.** The shader uses the previous pose only in that frame, so a pose that hasn't
  changed has no motion.
- **Velocity pass.** `bAlwaysHasVelocity` puts the skin in the velocity pass even though its transform never
  moves.
- **Setting.** It is on with **Motion Blur** (RigExec > Render).

**Ray tracing.** In a project with ray tracing enabled, each material section gets a ray-tracing geometry
(BLAS). It is built once and refit in place after every pose upload, which is about ten times cheaper than a
rebuild. The skin gives it to every visible view, so hardware ray-traced shadows, reflections and Lumen's
hardware path see the posed character. The example project leaves ray tracing off, as Unreal does by default.

## The actor

`ARigExecActor` is a plain root with two sibling components:

| Component | Class | Job |
| --- | --- | --- |
| `Rig` | `URigExecComponent` | Owns the runtime reader and the meshes, and turns control values into avars |
| `Controls` | `UControlRigComponent` | Runs the Control Rig asset that `build_control_rig.py` made, which is what Sequencer keys |

They are siblings rather than parent and child, which makes the tick order
explicit. In `OnRegister` the `Rig` component adds the Control Rig
component's tick as a prerequisite, and it ticks in `TG_PostUpdateWork`.
Each frame the Control Rig evaluates first, then the rig reads its result.
`bTickInEditor` is set, so posing works in the editor viewport without
Play-In-Editor.

## Loading

`URigExecComponent::Load()` (also called from `OnRegister` when the paths are
set, because a saved level keeps the paths, not the meshes):

1. Reads `RigExecFile` into memory and opens it with
   `RigExecRuntimeReader::Open`. Then it selects the first baked frame and
   runs once.
2. Opens `StageFile` through `UnrealUSDWrapper::OpenStage`.
3. For every point list in the runtime whose property is
   `<mesh>.points`, it finds the `UsdGeomMesh` at that path and builds a
   `FDynamicMesh3`:
   - vertices are the **rig's** points, so vertex *i* is always the rig's
     point *i*
   - faces are fan-triangulated from `faceVertexCounts`/`faceVertexIndices`
   - material slot 0 is the mesh's bound material, and each bound
     `GeomSubset` adds a slot that overrides it for its faces
   - per-vertex normals
4. Creates one transient `UDynamicMeshComponent` per mesh. They are never
   saved; they are rebuilt from the files on every load. Each material slot
   gets a `UMaterialInstanceDynamic` of `SurfaceMaterial`, or of the engine's
   `/RigExec/M_RigExecSurface` when that is empty (the engine's `BasicShapeMaterial` if the plugin's content is missing). The instance gets the
   `UsdPreviewSurface`'s `diffuseColor`, `roughness` and `metallic`, set as
   the `Color`/`BaseColor`, `Roughness` and `Metallic` parameters.
5. Reads `ControlsFile`, if set (see [Controls](#controls)).

## Evaluating

Each tick, `PullControls()`:

1. Reads every control's value relative to its offset from the Control Rig
   hierarchy.
2. Converts it back to the rig's basis (see
   [Coordinates](#coordinates-and-units)). It decomposes the rotation into
   Euler angles in the control's `rotationOrder`, picking the solution
   nearest the last sent values so angles unwrap continuously. It applies
   `avars:rotationSign` and divides translation by `avars:unitScaleFactor`.
3. Sends each transform avar (`avars:tx` … `avars:sz`) that the control
   carries, and each animation channel's value, through `SetAvar`. Only
   values that changed by more than 1e-5 are sent.
4. If anything changed, it calls `Evaluate()`. This runs `Execute` and writes
   the new points into each dynamic mesh. It recomputes normals from the
   deformed points, so skinning, blend shapes and every other deformer shade
   correctly.
5. Calls `PlaceControls()` (described below).

When nothing changed, nothing is evaluated, so an idle character costs only
the hierarchy reads.

### Controls follow the rig

After each evaluation the rig knows where every control actually is.
`RigExecRuntimeReader::GetControlFrame` returns it, and IK, space switches
and constraints all move controls. `PlaceControls` rewrites each control's
**offset** so that its gizmo lands on that frame:

```
offset = value⁻¹ · frame · parentFrame⁻¹
```

Controls are processed parents first. Helper transforms exported as nulls
are placed directly. The animator's value is never touched, so keys stay
what the animator set and only the drawing follows the rig.

### Undo

Unreal undoes a Control Rig edit by replaying control values, and Sequencer then re-evaluates its tracks. An
undo can therefore restore state that this component never saw change, such as the pose after undoing an IK/FK
switch. After every editor undo or redo (`FEditorDelegates::PostUndoRedo`), the component resyncs twice:
straight away, and again on the next tick, after Sequencer has had its turn. A resync sends every control and
channel value again whatever it held, re-poses the rig, re-places the gizmos and rewrites the Touch Pose
colours, which is what scrubbing does. `RequestResync()` does the same on demand.

### Gizmo placement cost

All 606 gizmos are placed after every pose. The root's global is read once per placement, and each parent's
global is taken from the rig's own frames (a placed parent sits on its frame) rather than from the hierarchy,
which would recompute it right after it moved. On the biped this cut placement from about 21 ms to
about 14 ms a pose.

### IK/FK visibility

A control with a `fade` in the controls file (a limb's IK or FK half) is
shown only while its fade channel is above zero (inverted for the other
half). This matches the display opacity usdview uses, so only the half that
drives the limb is grabbable. During a hand-over both halves stay visible.

## Controls

`Biped_controls.json` maps the Control Rig back to the rig. Each control
entry gives the rig path and the Control Rig `element` name, its parent
index, its rest `offset` (USD row-vector, asset space), `rotationOrder`,
`rotationSign`, `unitScale`, which of `translate`/`rotate`/`scale` it
carries, and its `defaults`. Each channel entry gives the owning control, the
`attr` (for example `avars:ikfk`), and its default, range and labels. `limbs`
describes each IK/FK limb for matching. See
[rig-preparation.md](rig-preparation.md#the-controls-file) for the full
format.

At load, each channel is tried against the runtime with `SetAvar`. Any it
refuses is dropped, and the dropped channels are logged in one line. A mode
switch the bake read as a constant is an example.

## Coordinates and units

**The Control Rig is in USD space.** The controls file is marked `"space": "usd"` by `build_control_rig.py`,
and the component refuses any other. It describes a Control Rig built without per-control conversion. Every control and null has its USD rest offset and value,
and one root null (`"root"`, `UsdSpace`) holds the Y/Z swap. The component then:

- reads a control's local value straight as the avars matrix, with no swap;
- finds a rig frame's Control Rig global as `frame × assetToWorld` (centimetres), then multiplied by the
  root's global from the hierarchy.

The swap is a mirror (determinant −1), so every global under the root is mirrored and locals are not. The
component therefore writes only locals: offsets, null transforms and `SwitchLimb`'s values
(`SetControlLocalTransform`). That way no `FTransform` has to carry the mirror.

Checked on the biped:
- the root reproduces the swap to 1e-15;
- every rest value equals its USD value exactly;
- gizmos sit on the rig's frames and within 0.05 cm of the default level's;
- an IK/FK switch moves the limb under 0.7 cm;
- setting a control's global round-trips: a +5 cm world-up drag becomes +5 in the control's local Y, with
  positive scale.

usdRig works in USD's convention: Y-up, right-handed, row vectors, in the
stage's `metersPerUnit`. Unreal is Z-up, left-handed, and in centimetres.

- **Axes.** Swapping Y and Z converts in both directions. Points use
  `UsdToUnreal::ConvertVector`. Control frames are never swapped one by one:
  the Control Rig's `UsdSpace` root carries the swap for all of them.
- **Winding.** USD faces wind counter-clockwise in a right-handed space. The
  Y/Z swap mirrors them, so the same index order is the front face in Unreal,
  and the index order is kept.
- **Units.** Points go through `UsdToUnreal::ConvertVector` with the stage's
  info. Frames and control translations scale by
  `CentimetresPerUnit = metersPerUnit × 100`.
- **Placement.** `assetToWorld` from the controls file carries the asset's
  placement in the stage. Control frames are asset-space and are multiplied
  by it, then by the `UsdSpace` root's global.

## The picker

`SRigExecPicker` (the **RigExec Picker** tab) finds the `ARigExecActor` in
the level. It reads the picker layout that `export_picker.py` wrote beside
the controls file (`Biped_controls.json` → `Biped_picker.json`) and draws
each panel with the button outlines usdview's picker paints. Those outlines
are triangulated at export time, so Slate draws plain polygons.

- Buttons select their Control Rig controls through the Control Rig, so the
  selection is the same one the viewport and Sequencer use.
- Attribute buttons write a channel. Space buttons call a space switch that
  keeps the control in place. IK/FK buttons call `URigExecComponent::SwitchLimb`.
- Every action is a `FScopedTransaction` (one undo step) and is keyed like a
  drag.

`FRigExecPickerModel` holds everything a picker view needs and nothing about
how it is shown:
- the loaded panels and which buttons are live;
- which buttons show (each IK/FK pair shows only the limb's current half);
- hit tests and marquees in panel units;
- what a click does;
- how a button paints, at any offset, scale and opacity.

The docked picker and the hover picker are two views of that one model, so
they cannot disagree about a button.

See [editor-workflow.md](editor-workflow.md#the-picker) for how to use it.

## The hover picker

`FRigExecHoverPicker` ports usdRig's `pickerHoverUI.py`, and
`RigExecHover::FLayout` (`RigExecHoverLayout.*`) ports `pickerHoverModel.py`
line for line: the handle and knob geometry, the fit scale, the gesture
arithmetic and the JSON layout. The design is usdview's:

- **Paint.** A paint-only `SLeafWidget` (`SRigExecHoverOverlay`,
  `HitTestInvisible`, volatile) sits on every level viewport through
  `SLevelViewport::AddOverlayWidget`. The mouse passes through it, the way
  usdview's overlay is mouse-transparent.
- **Input.** An `IInputProcessor`, registered first in line at editor
  start, decides which presses are the picker's. It claims those that land
  on a button, a handle or the knob, and only over a hovered level viewport,
  so menus and other windows in front keep their clicks. Everything else
  goes on to Touch Pose and the viewport. Alt with any button but the left
  is the camera's, and so is Alt anywhere but on a button.
- **Gestures.** A press it claims starts a gesture (move, scale, fade, knob
  or pick). The gesture takes the moves and the release until the button
  comes up. A release lost outside the editor ends it on the next tick.
- **Hover.** It is worked out every tick from the cursor. A move event
  arrives before Slate updates which viewport is hovered.
- **The P key.** The processor is installed whether the picker is shown or
  not, so P works at any time. It claims P only over a level viewport with a
  RigExec character in the level, and never into a text field.

The overlay is added to every level viewport while the picker is on. It is
re-added when a layout change remakes the viewports, and taken off when the
picker is turned off. Touch Pose asks `FRigExecHoverPicker::IsOver` before
it hovers or claims a press, so a picker button over the skin wins.

## The marking menu

`RigExecMarking` (`RigExecMarkingMenuModel.*`) ports usdRig's
`markingMenuModel.py`, and the Selection menu of `markingMenuDefs.py`, to
C++. It holds:
- the timings (150 ms to show, 200 ms tap, 250 ms dwell to unfold);
- the 45° sectors centred on the eight directions, with a 12-unit dead zone;
- the pinned-then-filled layout and the overflow column;
- hidden-mid-flick versus greyed-when-read;
- submenus with back circles;
- the press, move, tick and release state machine.

Time is passed in, so the thresholds are exact.

`FRigExecMarkingMenu` (`RigExecMarkingMenu.*`) is an `IInputProcessor`,
registered first in line at editor start:
- **The press.** It claims a right press when something under the cursor
  names a control, checked in this order: a hover-picker
  button (`FRigExecHoverPicker::ControlsAt`), a Touch Pose region
  (`FRigExecTouchPose::ControlAt`), then a control shape's hit proxy (an
  `AControlRigShapeActor` of a RigExec character's Control Rig). Anything
  else goes on to the viewport.
- **Modal while open.** Once open it takes every press, move and release
  until the gesture ends, and the release of the press that ended it.
- **Precedence.** A control under the cursor wins with any modifiers. With
  none under it, a plain right-click with controls selected opens the menu
  for them. Alt over empty space, or nothing selected, stays the camera's.
- **Painting.** The menu paints in a hit-test-invisible overlay that sits
  above every panel in the editor window the viewport is in
  (`SWindow::AddOverlaySlot`). It is added when the menu is first drawn, so a
  flick never adds one.
  - **Clamping.** Label boxes and the overflow column are moved, not
    resized, into that window's client area (within the monitor), and hit
    tests use the same boxes.
  - **Why not a separate window?** A separate per-pixel transparent window
    was tried first. The RHI renderer does not composite one with the
    desktop, so it drew black.
- **The selection.** It is read as controls only: the animation channels
  Control Rig sometimes selects alongside a control are dropped.

The commands go through the Control Rig with undo:
- **Key** sets each control and channel to its current value with
  `EControlRigSetKey::Always`, which Sequencer's Control Rig track keys.
- **Reset to rest and Zero pose** set initial values.
- **The FK/IK switch and Space** use `URigExecComponent::SwitchLimb` and
  `SwitchSpace`, with the limb and space found by `FindLimbSwitchFor` and
  `FindSpaceChannel`.
- **Frame** uses `MoveViewportCamerasToBox`.
- **Viewport menu** uses `ILevelEditor::SummonLevelViewportContextMenu`.

## Touch Pose

The editor's Touch Pose mode (`RigExecTouchPose.cpp`) ports usdview's
TouchPose plug-in (`usdRig/plugin/touchPose`) onto the dynamic meshes.

- **Regions.** At load, `URigExecComponent::LoadTouchRegions` finds every
  `RigExecTouchRegions` scope by type. It keeps each mesh's first layer
  (ordered by `layerOrder`, then label, then path) and maps each USD face to
  its region. Typed `rigExec:touch:*` properties win, and the older
  `touchpose:*` ones are the fallback, as in `touchPoseModel.py`. The loader
  already records which USD face each triangle came from, so a picked
  triangle maps straight to a face and then a region.
- **Picking.** A `FDynamicMeshAABBTree3` per touched mesh is built on the
  posed skin. `Evaluate` marks it dirty and the next pick rebuilds it, so a
  click lands on the skin as posed. Unpainted skin still occludes what is
  behind it.
- **Input.** An `IInputProcessor`, registered only while the mode is on,
  ticks hover from the cursor (skipped while a mouse button is held). It
  never takes a click a manipulator could have. With the new TRS gizmos
  (`UEditorInteractiveGizmoManager::UsesNewTRSGizmos`), it asks the level
  editor's `UTransformGizmo` through `CanBeginClickDragSequence`, the test
  its input router runs on a press, because that gizmo does its own ray test
  and draws no hit proxies. It also checks the legacy widget
  (`GetCurrentWidgetAxis()`) and scans the hit-proxy map 6 px around the
  cursor for `HWidgetAxis` or an `AControlRigShapeActor`. It then takes the click
  only when the hit proxy at the pixel is the character itself.
- **Suspension.** Each tick, a character counts as busy while the left button
  is held (excluding Touch Pose's own click) or within 0.35 s of its last
  pose (`URigExecComponent::GetLastPoseTime`, stamped in `Evaluate`).
  `SetTouchSuspended` clears the tint without forgetting the highlight;
  resuming writes it back.
  It selects through `UControlRig::SelectControl` in a transaction (Shift
  toggles, Ctrl removes), and swallows the matching release.
- **Highlight.** The tint is written into the drawn meshes' vertex colours.
  Each face corner has its own colour element, untinted at alpha 0, and only
  the faces of regions whose state changed are rewritten (an attribute edit
  with the `VertexColors` flag). The surface material blends the colour in by
  its alpha at usdview's 30% (`HIGHLIGHT_OPACITY`), with a little emissive so a
  lit region reads in shadow. Because the tint lives on the mesh, it moves
  with the pose at no cost and reaches a subdivision surface that draws the
  meshes (see [subdivision.md](subdivision.md)). Colours follow
  `touchPoseModel.py`: hover is the region's palette entry
  (`touchpose:hilight` indexes `rigExec:touch:palette`) scaled to full value;
  lead and selected are the scope's `touchpose:leadColor`/`selectedColor`
  (green and grey by default) scaled to 0.95 and 0.62.

## IK/FK matching

`SwitchLimb` hands a limb to its other half without moving it:

- **To FK:** each FK control is put on `fkOffset × joint`, root to end,
  evaluating after each one so children follow.
- **To IK:** the IK control is placed so the effector lands on the end joint
  (`effectorOffset⁻¹ × endJoint`). Any roll stack between the control and
  the effector is carried as it is. The pole goes onto the bend plane, at the
  rest distance, on the side the middle joint bends toward. A straight limb
  keeps the pole's direction.
- Then the switch channel flips. The offsets are measured at rest by
  `export_controls.py`, using usdRig's own `ikfkMatch` model.
