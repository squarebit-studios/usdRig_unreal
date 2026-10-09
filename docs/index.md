# RigExec for Unreal

The **RigExec plugin** brings [usdRig](https://github.com/squarebit-studios/usdRig) characters into
**Unreal Engine 5.8**. You pose and animate them with Control Rig and Sequencer, using the same rig
usdview evaluates. Nothing is rebuilt or approximated in Unreal.

The rig is baked once, outside Unreal, into a `.rigexec` file. The plugin evaluates that file with RigExec's
binary runtime, a USD-free C++17 library linked into the plugin. Every change to a control re-poses the rig
and deforms the meshes in place, on the GPU.

```
 usdRig (outside Unreal)                          Unreal Editor (this plugin)
 ------------------------                         ----------------------------------------
 Biped_stack.usda --rigExecBake--------> Biped.rigexec -----> RigExec runtime (poses the rig)
                  --export_controls.py-> Biped_controls.json -> Control Rig + control map
                  --export_picker.py---> Biped_picker.json --> Docked and hover pickers
 Biped_stack.usda (meshes, materials, touch regions) ------> GPU-skinned meshes, Touch Pose
```

## What you get

<div class="grid cards" markdown>

- **The rig, exactly**

    The baked rig evaluates in Unreal as it does in usdview: IK/FK limbs, spaces, blend shapes, wires and
    every other deformer.

- **A USD-space Control Rig**

    Every control keeps its USD offset and value as authored. One root converts the whole asset to Unreal's
    space, so a control's value in Anim Details is the avar usdview shows, and animation always matches.

- **Drawn on the GPU**

    Normals and tangents come from a compute shader, with motion vectors for motion blur and TSR, and
    ray-tracing support. If the GPU path isn't available, a CPU fallback takes over, and the component says
    so with a banner and a toast.

- **Animator tools from usdview**

    - The **picker**, docked or drawn right in the viewport.
    - **Touch Pose**, for selecting controls by touching the skin.
    - The right-click **marking menu**: key, reset, zero, frame, IK/FK, spaces and mirror selection.

- **Sequencer and undo**

    Keys go on a Control Rig track like any other rig. Undo and redo bring the rig back exactly, and IK/FK and
    space switches keep the limb where it is.

- **Works with Squarebit Subdivs**

    Add [Squarebit Subdivs](https://www.squarebitstudios.com/squarebit-subdivs) and the character draws as a subdivision surface on the GPU,
    with Touch Pose, the pickers, Sequencer and undo working as before. It's optional: RigExec doesn't need it.
    See [Subdivision](editor-workflow.md#subdivision-with-squarebit-subdivs).

</div>

## Quick start

You need Windows, Unreal Engine 5.8, and Visual Studio 2022 with the C++ workload. Put a usdRig checkout
beside this repository (`..\usdRig`); the example reads its meshes from there.

```bat
rem 1. Build the plugin and install it into the example project.
build_plugin.bat

rem 2. Open the example: the biped on a lit floor, with its Control Rig and a Level Sequence.
Examples\RigExecBiped\Launch_RigExecBiped.bat
```

In the editor:

1. Open `LS_RigExec` from `Content/RigExec`. It holds the character's Control Rig track.
2. Select controls in the viewport, open **Tools → RigExec → Picker**, or press **P** over the viewport for
   the hover picker.
3. Move a control. The rig evaluates and the meshes follow.
4. Right-click a control for the marking menu, and key it like any Control Rig.

[Working in the editor](editor-workflow.md) covers every tool in detail.

## Bringing your own character

1. **Prepare the rig** with usdRig: bake the `.rigexec` and export the controls and picker files.
   See [Preparing a rig](rig-preparation.md).
2. **Spawn it** in a project with the RigExec plugin. A setup script builds its USD-space Control Rig and
   places the character. See [Working in the editor](editor-workflow.md#setting-up-your-own-character).
3. **Animate** with Control Rig and Sequencer.

## Where to go next

| If you want to | Read |
| --- | --- |
| Build the plugin, or refresh the vendored runtime | [Building and installing](building.md) |
| Get a usdRig character ready for Unreal | [Preparing a rig](rig-preparation.md) |
| Use the pickers, Touch Pose, the marking menu, Sequencer, IK/FK and spaces | [Working in the editor](editor-workflow.md) |
| Draw the character as a subdivision surface | [Subdivision](editor-workflow.md#subdivision-with-squarebit-subdivs), with [Squarebit Subdivs](https://www.squarebitstudios.com/squarebit-subdivs) |
| Fix something that isn't working | [Limits and troubleshooting](troubleshooting.md) |
| Script the plugin from Blueprint, Python or C++ | [API reference](api.md) |
| Understand how it works inside | [Architecture](architecture.md) |
