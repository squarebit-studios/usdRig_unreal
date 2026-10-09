# Working in the editor

## The example project

`Examples/RigExecBiped` is a content-only UE 5.8 project. It enables
RigExec, ControlRig, USDCore and the Python Editor Script Plugin, and it
opens on `/Game/RigExec/L_RigExec`.

| Asset | What it is |
| --- | --- |
| `L_RigExec` | The level: the `Biped` (`ARigExecActor`), a floor, movable sun and sky lights, a sky atmosphere, and `BipedSequence` (a Level Sequence Actor) |
| `CR_Biped` | The Control Rig made from `RigExecData/Biped_controls.json` |
| `LS_RigExec` | A 0–120 frame Level Sequence with the biped's Control Rig track |

The rig data is in `RigExecData/` and is read relative to the project. The
meshes are read from `../../../usdRig/examples/biped/Biped_stack.usda`,
which is the usdRig checkout beside this repository.

To open it:

```bat
build_plugin.bat                                   & rem once, and after any C++ change
Examples\RigExecBiped\Launch_RigExecBiped.bat
```

All lights are movable, so the project opens without a lighting build.

### Rebuilding the example project

Rebuild after `prepare_biped.bat`, or whenever the controls file changes:

```bat
Examples\RigExecBiped\Setup_RigExecBiped.bat
```

This launches the editor with `Scripts\setup_example_project.py` as a
project startup script. The script is passed through a command-line `-ini:`
override, so no config file changes. It waits for the editor to settle,
rebuilds and saves `CR_Biped`, `L_RigExec` and `LS_RigExec`, then quits. The
batch file checks the log
(`Examples\RigExecBiped\Saved\Logs\RigExecBiped.log`) for `RIGEXEC_SETUP_OK`
and reports a failure otherwise. Close any open editor on the project first.

Don't use `-ExecutePythonScript` for this. The editor exits before the
script's tick callback fires, so nothing gets built.

### The Control Rig is in USD space

`CR_Biped`'s controls are the rig's own: every control and null keeps its USD rest offset and value exactly
as authored, in centimetres. A single null, `UsdSpace`, above the rig's top controls carries the whole asset
from USD's Y-up right-handed space into Unreal's Z-up one; that conversion (the Y/Z swap) happens there and
nowhere else. `build_control_rig.py` writes `"space": "usd"` and the root's name into the controls file, and
the RigExec component accepts only such a file.

A control's value in Anim Details is therefore the avar as usdview shows it, so animation always matches the
rig. Because the swap is a mirror, the gizmos' world frames are mirrored too: a control's local Y is world up.

## Setting up your own character

You need a prepared rig ([rig-preparation.md](rig-preparation.md)) and a
project with **RigExec**, **ControlRig**, **USDCore** and **Python Editor
Script Plugin** enabled.

From the editor's Python console (Output Log, set to `Python`), or with
`py <script> <args>` from the `Cmd` console:

```text
py <repo>/Scripts/build_control_rig.py <Name>_controls.json /Game/RigExec/CR_<Name>
py <repo>/Scripts/spawn_rig.py <Name>.rigexec <stage.usda> <Name>_controls.json /Game/RigExec/CR_<Name>
```

**`build_control_rig.py`** creates a Control Rig Blueprint:

- each animator control becomes a **Euler-transform control** with the
  rig's rotation order, guide shape and colour, placed at its rest offset
  converted to Unreal space
- each rig helper becomes a **null**, so the hierarchy is the rig's own
  without exposing what the rig drives
- each other avar becomes an **animation channel** under its control. A
  float slider spans the usdview Avar Editor's range for that attribute: its
  authored `limits`, else 0 to 1. It clamps only to the rig's hard limits. A
  space switch is an index from 0 to the last space, labelled with the space
  names
- it writes the generated element names back into the JSON as `"element"`,
  which is how `URigExecComponent` finds them. **Run it before
  `spawn_rig.py`**, and again whenever the JSON is regenerated

**`spawn_rig.py`** places an `ARigExecActor` at the origin. It points its
`Controls` component at the Control Rig, sets the `Rig` component's three
file paths and calls `Load()`. It raises an error if the load fails, and the
reason is in the log under `LogRigExec`.

Paths can be absolute or relative to the project directory. Use relative
paths, so the level opens from any checkout.

### Doing it by hand

1. Place a **RigExec Actor** (Place Actors → All Classes).
2. In **Rig** (the `RigExecComponent`), set `RigExecFile`, `StageFile` and
   `ControlsFile`. Optionally set `SurfaceMaterial`.
3. In **Controls** (the `ControlRigComponent`), set **Control Rig Class** to
   the asset `build_control_rig.py` made.
4. Call **Load** on the Rig component (Blueprint, Python
   `actor.rig.load()`), or reopen the level. `OnRegister` loads when both
   file paths are set.

## Animating

1. Open the Level Sequence and add the actor, if it is not bound already.
2. On the actor's track: **+ Track → Control Rig → the `Controls`
   component**. In Python:
   `unreal.ControlRigSequencerLibrary.find_or_create_control_rig_component_track(world, sequence, binding)`.
3. Opening the sequence enters the Animation mode. Select controls in the
   viewport, the Animation Outliner or the [picker](#the-picker), then
   transform and key them as with any Control Rig. Animation channels
   (`ikfk`, `space`, face sliders) appear under their control in Sequencer
   and in **Anim Details**.

The rig evaluates whenever a control or channel changes, in the editor
viewport, in Sequencer scrubbing and playback, and in PIE. A pose costs one
runtime `Execute` plus the mesh update. `URigExecComponent::Evaluate()`
returns that time in milliseconds.

**Controls As Overlay** is on in the example
(`DefaultEditorPerProjectUserSettings.ini`). Most of a body rig's controls
sit inside the body, so without it they would be hidden.
`URigExecRuntimeLibrary::SetControlsAsOverlay` sets it from script. Set it
before opening a sequence, because the mode reads it on start.

### Gizmos follow the rig

After every evaluation each control's **offset** is moved onto the frame
the rig computed for it. Gizmos therefore track IK, space switches and
constraints exactly, while the **value** you key stays your own. If you
inspect a control's offset transform, expect it to change as you pose.

### Mode switches

- **IK/FK** (`ikfk`) is an ordinary float channel: 0 is one half and 1 the
  other. The value meaning IK is per limb (`ikValue` in the controls file).
  Keying it between the two values blends. The half not driving the limb is
  hidden, in the viewport and in the picker, matching usdview's display
  opacity.
- **Space** (`space`) is the active index into the control's spaces, and is
  labelled with them. A value between two indices blends.
- Setting these channels directly snaps the limb or control to wherever the
  new mode puts it. Use the **picker's switch buttons** to switch without a
  pop.

## The picker

**Tools → RigExec → Picker** (or the console command
`RigExec.OpenPicker`) opens the **RigExec Picker** tab. It also appears
under **Window → Level Editor**.

The picker binds to the **first `ARigExecActor` in the editor world**. It
reads `<Name>_picker.json` from beside the rig's controls file, and draws
the rig's own picker panels as usdview draws them. The status line says when
no picker file is found.

| Gesture | Effect |
| --- | --- |
| Click | Select the button's controls |
| Shift + click | Add or remove them |
| Ctrl + click | Remove them |
| Alt + click | Also take the mirrored button's controls (L ↔ R) |
| Drag | Select every button the marquee touches |
| Click an **IK/FK** button | Match the incoming half to the limb's current pose, then switch, so the limb does not move |
| Click a **space** button | Choose a space from its menu. The control stays where it is |
| Click an **attribute** button | Set that channel (labels show its value) |
| **Zero controls** | Return the selected controls and their channels, or the whole rig when nothing is selected, to their initial values |
| **Controls visibility** | Toggle the rig's control gizmos |

Each action is one undo step, and it keys the same way a drag in the
viewport would (that is, according to Sequencer's auto-key setting).

## The hover picker

The hover picker is a port of usdview's hover picker. It draws the picker's
tabs straight into the level viewport as floating buttons, in screen space.
Turn it on with **Tools → RigExec → Hover Picker**, the console command
`RigExec.HoverPicker 1` / `0`, or **P** with the cursor over a level
viewport.

There is no window around the buttons. Anywhere that is not a button, a
handle or the opacity knob stays the viewport's: a click there selects,
box-selects, drags a gizmo or moves the camera as usual.

Each tab hangs off a small round handle. The first time it is shown, the
first tab opens and the rest wait collapsed in a row under the viewport
toolbar.

| Gesture | Effect |
| --- | --- |
| Drag a handle | Move the tab; its buttons follow |
| Shift + drag a handle | Scale the tab |
| Double-click a handle | Collapse the tab to its handle, or expand it |
| Hover a handle | Show the tab's name |
| Click, Shift / Ctrl / Alt + click, or drag on the buttons | The same selections and switches as the docked picker. A drag box-selects within that tab |
| Middle-drag a tab's buttons or handle, left or right | Fade that tab |
| Drag the knob at the bottom right, left or right | Fade every tab at once; double-click it to go back to full |

Only the buttons are drawn: the docked picker's backdrops stay in the docked
picker, where they don't hide the scene. Both pickers share one model, so a
button behaves the same in either, and the hover picker's buttons win over
Touch Pose underneath them.

The layout (where each tab is, its scale, opacity and whether it is
collapsed) and whether the hover picker is on are remembered per user in
`Saved/RigExec/HoverPicker.json`. They are never saved into the level.

**P** toggles the hover picker only while the cursor is over a level
viewport, a RigExec character is in the level, and no text field has focus.
Everywhere else P keeps its usual meaning. Over the viewport it replaces the
Navigation show-flag toggle.

## The marking menu

The marking menu is a port of usdview's right-click menu for rig controls.
Right-click over a control and a radial menu acts on the selection. A
control here means its shape in the viewport, a Touch Pose region (which
stands for its control), or a hover-picker button.

- **Flick** toward a direction and release: the command runs, and nothing
  is ever drawn.
- **Hold** for a moment and the ring appears around the press point;
  release over an item to run it.
- **Tap** (a quick press and release without moving) and the ring stays up
  to be clicked. Items that don't apply to the selection are greyed out
  rather than hidden.
- **Escape**, or a click away from the ring, dismisses it.
- A **submenu** opens when the stroke passes through it or rests on it.
  Each menu above leaves a circle where it was centred; move back onto a
  circle to return to that menu.

| Direction | Item | Does |
| --- | --- | --- |
| W | **Key** | Keys the controls and their channels at the current frame. Needs the level sequence open |
| E | **Reset to rest** | Puts the controls' transforms back to rest |
| S | **Zero pose** | Puts the controls and all their channels (IK/FK, space, foot roll…) back to their initial values |
| N | **Frame** | Frames the controls in the viewport |
| NW | **Switch to IK / FK** | Matches and switches the limb the selection is on. Only for limb controls |
| NE | **Space ›** | The lead control's spaces; the current one is checked. Switching keeps the control where it is |
| SW | **Select counterpart** | Selects the mirrored controls (L_ ↔ R_ and the like) |
| column | **Viewport menu…** | Opens Unreal's own viewport context menu |

Every slot is pinned, so a direction always means the same command.
Right-clicking a control that isn't selected selects it first, so the menu
always acts on what is highlighted. Each command is one undo step.

Controls take precedence over the camera, because a right-click on one is
intentional:
- **Over a control** the menu opens whatever the modifiers, Alt included.
- **With controls selected** a plain right-click anywhere in the viewport
  opens the menu for them.
- The camera keeps the right button only when nothing is selected, or with
  **Alt over empty space** (the dolly). Deselect, or hold Alt, to fly.

The menu draws over the whole editor window, above every panel, so the
viewport's edge never cuts it off. A label that would run off the window
slides back on, and it is clicked where it is drawn.

## Touch Pose

**Tools → RigExec → Touch Pose** (or the console command
`RigExec.TouchPose 1` / `0`) turns the character's skin into the picker, as
usdview's TouchPose does:

| Gesture | Effect |
| --- | --- |
| Hover the skin | Lights the touch region under the cursor in its palette colour |
| Click a region | Selects the control the region names |
| Shift + click | Adds or removes it |
| Ctrl + click | Removes it |

Regions whose controls are selected stay lit. The most recently selected
control's region is green (the lead colour) and the rest are grey, so a
selection made in the viewport, the outliner or the picker shows on the
skin too.

**Manipulators always win.** A click goes to Touch Pose only when no
manipulator is at or near the cursor. That rules out the transform gizmo
(the newer Interactive Tools Framework gizmo is asked directly whether it
would take the press; the legacy widget counts when hovered or within 6
pixels) and any Control Rig control shape within 6 pixels. It also has to land on a
painted region, with the character's skin the frontmost thing under the
cursor. Anything else (a gizmo, the widget, another object, unpainted skin)
goes to the viewport as usual, and Alt+click (camera orbit) is never taken.
Hovering the transform widget shows no region highlight.

**It steps aside while you work.** Touch Pose turns itself off, hiding its
highlight and ignoring clicks, in two cases:
- the left mouse button is held anywhere: dragging a gizmo, dragging a
  slider in Anim Details or the Details panel, or scrubbing the timeline
- the rig has re-posed in the last 0.35 s: playback, typing a value,
  changing a key, a picker switch

It comes back, with the selection highlight on the skin as it is posed now,
as soon as the button is released and the rig has been still for 0.35 s.

The regions come from the stage: each mesh's first `RigExecTouchRegions`
layer (by `rigExec:touch:layerOrder`), as usdview opens it. On the biped
that means 219 regions over the body and both eyes. A region whose control
is a rig helper (a Control Rig null) is skipped and logged. The highlight is a tint in the meshes' vertex colours, blended by the
surface material `/RigExec/M_RigExecSurface` (made by
`Tools/make_materials.py`). It therefore shows on a subdivided character too.
A `SurfaceMaterial` of your own must
blend the vertex colour in the same way for the highlight to show.

## Subdivision with Squarebit Subdivs

RigExec works with [Squarebit Subdivs](https://www.squarebitstudios.com/squarebit-subdivs), which draws a mesh as a
Catmull-Clark subdivision surface on the GPU. With it installed in your project:

1. Select the RigExec actor.
2. **Add Component → Squarebit Subdiv Dynamic Mesh Component**.
3. Set **Subdivision Level** (1 is the usual choice).

The component gathers the character's meshes, hides them, and draws them
subdivided, following every pose live. Touch Pose, the pickers, the marking
menu, Sequencer and undo work as before, and Touch Pose highlights keep hard
edges on the subdivided surface.

Squarebit Subdivs is optional and isn't part of this repository: RigExec runs
without it. See the [Squarebit Subdivs site](https://www.squarebitstudios.com/squarebit-subdivs) for how to get it.

## Scripting and testing

`URigExecRuntimeLibrary::RigExecSelfTest(file, channel, values)` opens a
`.rigexec` inside Unreal's process, poses one channel through a list of
values, and reports open time, per-pose evaluation time and how far points
moved. It is useful for checking a new bake or a runtime update without any
Control Rig:

```python
print(unreal.RigExecRuntimeLibrary.rig_exec_self_test(
    "C:/.../Biped.rigexec",
    "/Biped/Rig/Main/Shot/Aux/Controls/M_Body/.../M_Jaw.avars:rx",
    [0.0, 10.0, 20.0]))
```

The full API is in [api.md](api.md).
