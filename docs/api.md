# API reference

Everything below is in the `RigExecRuntime` module (`RIGEXECRUNTIME_API`)
unless noted otherwise. Blueprint names are as shown. Python uses
snake_case (`rig.set_channel(...)`).

Channel and control paths are **rig prim paths**, exactly as usdview shows
them. A control is `/Biped/Rig/.../L_Arm`, and a channel is that path plus
`.avars:<name>`, for example `/Biped/Rig/.../L_Arm.avars:ikfk`.

## `ARigExecActor`

`Public/RigExecActor.h`: a character ready for Sequencer.

| Member | |
| --- | --- |
| `URigExecComponent* Rig` | The rig and its meshes |
| `UControlRigComponent* Controls` | The Control Rig that poses it. `Rig->ControlRig` points here |
| `void SetControlRigAsset(UObject* Asset)` | Points `Controls` at a Control Rig Blueprint, or at its generated class |

Both components are attached to a plain `Root`. The actor itself does not
tick.

## `URigExecComponent`

`Public/RigExecComponent.h`, a `USceneComponent`. It ticks in
`TG_PostUpdateWork`, after the Control Rig component, and ticks in the
editor too.

### Properties (category *RigExec*)

| Property | Type | |
| --- | --- | --- |
| `RigExecFile` | `FFilePath` | A poseable `.rigexec`. Relative paths are relative to the project directory |
| `StageFile` | `FFilePath` | The USD stage the rig was baked from, read for mesh topology and materials |
| `SurfaceMaterial` | `UMaterialInterface*` | Parent of each mesh material. It receives `Color`/`BaseColor`, `Roughness` and `Metallic` parameters. Empty means `/RigExec/M_RigExecSurface`, which also blends the vertex colour in by its alpha (the Touch Pose tint, see [subdivision.md](subdivision.md#your-own-materials)) |
| `ControlsFile` | `FFilePath` | The controls description (after `build_control_rig.py`). Optional: without it, nothing maps the Control Rig onto the rig |
| `ControlRig` | `UControlRigComponent*` | The Control Rig that poses the rig. Empty means the owner's first one |

### Functions

| Function | Blueprint | |
| --- | --- | --- |
| `bool Load()` | ✓ | Opens the rig, runs its first frame, builds the meshes from the stage and reads the controls. Returns false, with the reason logged to `LogRigExec`, on failure. Called from `OnRegister` when both file paths are set |
| `bool SetChannel(const FString& Channel, float Value)` | ✓ | Sets one avar by path. It takes effect at the next `Evaluate`. Returns false (with a warning) when the runtime refuses it |
| `void ClearChannels()` | ✓ | Drops every avar that was set, back to the baked pose, and resets the cached control values so the next pull resends them |
| `float Evaluate()` | ✓ | Runs the rig and moves the meshes. Returns milliseconds, or negative on failure |
| `int32 PullControls()` | ✓ | Reads the Control Rig into avars and evaluates if anything moved, then re-places the control offsets. Returns the number of avars that changed. Called every tick |
| `TArray<UDynamicMeshComponent*> GetMeshes() const` | ✓ | The meshes `Load` built, in the rig's mesh order |
| `FVector GetVertex(int32 Mesh, int32 Index) const` | ✓ | A vertex as last placed, in Unreal space (for tests) |
| `bool SwitchLimb(const FString& SwitchPath)` | ✓ | Hands the limb switched by `SwitchPath` (`….avars:ikfk`) to its other half without moving it, then flips the switch. It writes through the Control Rig with undo, so wrap it in a transaction. Returns false if there is no such limb |
| `FString FindLimbSwitchFor(FName Element) const` | C++ | The switch (`….avars:ikfk`) of the limb a Control Rig control belongs to: the switch's control, an FK or IK control, the pole, or anything under the IK control. Empty when it is on no limb |
| `bool IsLimbIk(const FString& SwitchPath) const` | C++ | Whether that limb is on its IK half now |
| `bool FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const` | C++ | The control's space channel and its space names, from the controls file. False when it has fewer than two spaces |
| `bool SwitchSpace(FName Element, int32 Index)` | C++ | Moves the control to that space without moving it. It writes through the Control Rig with undo, so wrap it in a transaction |
| `FTransform GetRigFrame(const FString& Path) const` | ✓ | The posed frame of a control or joint, in the Control Rig's space. Identity if the path is unknown |
| `FName FindControlElement(const FString& Path) const` | C++ | The Control Rig control for a rig control |
| `FName FindChannelElement(const FString& AttrPath) const` | C++ | The Control Rig control or channel for a rig attribute |
| `FString PickerFilePath() const` | C++ | `<Name>_picker.json` beside the controls file, or empty |
| `bool IsLimbSwitch(const FString& SwitchPath) const` | C++ | Whether `SwitchPath` is a limb switch this rig can match |

### Example: posing without Control Rig

```cpp
URigExecComponent* Rig = Actor->Rig;
Rig->ClearChannels();
Rig->SetChannel(TEXT("/Biped/Rig/.../M_Jaw.avars:rx"), 15.f);
const float Ms = Rig->Evaluate();
```

```python
rig = actor.get_editor_property("rig")
rig.set_channel("/Biped/Rig/.../M_Jaw.avars:rx", 15.0)
print(rig.evaluate(), "ms")
```

Each tick also calls `PullControls()`, which re-sends any Control Rig value
that differs from what it last sent. To drive a character purely from
script, either leave `ControlsFile` empty or set `ControlRig` to none.

### Render path (category *RigExec|Render*)

| Member | Blueprint | |
| --- | --- | --- |
| `bDrawOnGPU` | property | Draw the meshes with the GPU skin (default). Off, or where it cannot run, the CPU fallback draws them. Changing it in Details rebuilds the meshes |
| `bMotionBlur` | property | On the GPU, write motion vectors from the pose's movement for TSR and motion blur (default on). Changing it rebuilds the meshes |
| `RenderPath` | read-only property | How the meshes draw now: `GPU: …`, or `CPU fallback: <reason>` |
| `bool IsDrawingOnGPU() const` | ✓ | Whether the GPU skin draws right now |
| `static FOnRenderFallback OnRenderFallback` | C++ | Broadcast `(Component, Reason)` when a component that wanted the GPU falls back. The editor turns it into a toast |

`URigExecSkinComponent` is the GPU skin: one per mesh, created and fed by the Rig component, not meant to be
added by hand.

### Touch regions (category *RigExec|Touch*)

| Member | Blueprint | |
| --- | --- | --- |
| `int32 GetTouchRegionCount() const` | ✓ | Regions read at load, numbered across the meshes |
| `FString GetTouchRegionName(int32 Region) const` | ✓ | The region prim's name, for example `R_LoArm_touch` |
| `FName GetTouchRegionControl(int32 Region) const` | ✓ | The Control Rig control the region selects |
| `int32 PickTouchRegion(FVector RayOrigin, FVector RayDirection, double& OutDistance)` | ✓ | The region a world-space ray first hits on the *posed* skin, or -1 if it misses or hits unpainted skin. The bounding-volume tree it uses is rebuilt lazily after the rig moves |
| `TArray<int32> GetTouchRegionsOf(const TArray<FName>& Controls) const` | C++ | Regions whose control is in the list |
| `void SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected)` | ✓ | Lights the hover, lead and selected regions (−1/empty for none) by writing their faces' vertex colours, and only for regions whose state changed. The tint rides the skin as it poses, and reaches a subdivision surface that draws the meshes |
| `void SetTouchSuspended(bool bSuspended)` / `bool IsTouchSuspended() const` | ✓ | Clears the tint without forgetting the highlight. Resuming writes it back |
| `double GetLastPoseTime() const` | ✓ | `FPlatformTime::Seconds()` of the last `Evaluate`, or 0 |

```python
rig = actor.get_editor_property("rig")
region, distance = rig.pick_touch_region(unreal.Vector(0, -400, 120), unreal.Vector(0, 1, 0))
print(rig.get_touch_region_name(region), rig.get_touch_region_control(region))
```

## `URigExecRuntimeLibrary`

`Public/RigExecRuntimeLibrary.h`: static Blueprint and Python helpers.

| Function | |
| --- | --- |
| `FString RigExecSelfTest(const FString& RigExecFile, const FString& Channel, const TArray<float>& Values)` | Opens a `.rigexec` and poses `Channel` once per value. Reports whether property chains are live, each pose's time, and the largest point move from the first pose. Needs no stage and no Control Rig |
| `bool ShowControlRigInViewport(UControlRig* ControlRig)` | *Editor only.* Enters the Control Rig animation mode, hands it this rig and shows its controls, which is the state the Animation Outliner's eye toggles. It loads the rig's shape libraries first, or the gizmos have no mesh |
| `bool SetControlsAsOverlay(bool bOverlay)` | *Editor only.* Sets the Animation mode's "Controls As Overlay" (`ControlRigEditModeSettings.bShowControlsAsOverlay`) and saves it. Set it before opening a sequence |

## Editor module (`RigExecEditor`)

| Entry point | |
| --- | --- |
| Tab `RigExecPicker` | "RigExec Picker", a nomad tab in the Level Editor's Window menu |
| **Tools → RigExec → Picker** | Opens the tab |
| Console `RigExec.OpenPicker` | Opens the tab |
| **Tools → RigExec → Hover Picker** | Toggles the hover picker over the level viewports |
| Console `RigExec.HoverPicker [0\|1]` | Shows or hides the hover picker; toggles with no argument |
| **P** over a level viewport | Toggles the hover picker (only with a RigExec character in the level) |
| Right-click over a control | Opens the marking menu (always on; see editor-workflow.md) |
| **Tools → RigExec → Touch Pose** | Toggles Touch Pose in the level viewports |
| Console `RigExec.TouchPose [0\|1]` | Turns Touch Pose on or off; toggles with no argument |

`SRigExecPicker`, `FRigExecPickerModel` and `FRigExecHoverPicker` are
private to the module. The hover picker's layout is saved per user in
`Saved/RigExec/HoverPicker.json`.

## Log category

`LogRigExec` (in `RigExecComponent.cpp`). On a successful load it logs:

```
loaded <file>: <n> meshes, <n> controls, <n> channels, property chains live|replayed
<n> of <m> channels are not live in <file>: <attr> on <control>, ...
```

## The runtime (`RigExecLib`)

The vendored `rigExec::RigExecRuntimeReader`
(`ThirdParty/RigExecLib/include/rigExecRuntime/runtime.h`) is usdRig's API.
The plugin uses this subset of it:

| Call | Use |
| --- | --- |
| `static Open(bytes, size, &error)` | Opens a `.rigexec` image held in memory |
| `GetFrameTimes()` / `SetFrame(t, &error)` | Selects the base frame |
| `SetAvar(path, value, &error)` / `ClearAvars()` | Live inputs |
| `Execute(&error)` | Evaluates |
| `GetPoints()` | Per-mesh deformed points (`path` is `<mesh>.points`) |
| `GetControlFrame(path, double[16])` | A control or joint's posed frame, asset space, row-vector |
| `HasPropertyChains()` | Whether the bake was `--poseable` |

It also offers `SetExternalKernel` for plugin movers, joint matrices,
diagnostics and counters. See the header comments.
