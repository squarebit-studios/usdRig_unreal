# Limits and troubleshooting

## Known limits

| Limit | Why |
| --- | --- |
| **Editor only** | Meshes are read from the USD stage, and Unreal builds USD into the editor only. In a build without `USE_USD_SDK`, `Load()` logs `this build has no USD; the meshes cannot be read`. The module still compiles for game targets, so a project that ships with the plugin enabled still packages |
| **Win64 only** | The vendored runtime libraries are MSVC builds (`PlatformAllowList: Win64`) |
| **No UVs or textures** | Meshes carry positions, per-vertex normals and material IDs only. Materials get the `UsdPreviewSurface` constants (`diffuseColor`, `roughness`, `metallic`). A procedural shader, such as the eye's iris, falls back to grey (0.7, roughness 0.6) |
| **One base frame** | The component selects the first baked frame and poses from avars. Baked animation in the `.rigexec` is not played |
| **Plugin movers pass through** | No external kernels are installed, so revision opcode 16 movers do not deform |
| **The picker binds to one actor** | It uses the first `ARigExecActor` in the editor world |
| **Normals are smooth** | Per-vertex normals are recomputed after each pose. Hard edges and authored normals are not kept |

## "RigExec is drawing on the CPU"

A toast, and a banner at the top of the Rig component's Details, say the meshes are drawing on the CPU
fallback. **Render Path** gives the reason:

| Reason | What to do |
| --- | --- |
| Draw On GPU is off for this component | Turn **Draw On GPU** on (RigExec > Render) |
| this renderer has no compute shaders (below Shader Model 5) | Run on a Shader Model 5 renderer (DirectX 12, Vulkan or Metal), not a mobile or ES preview |
| the RigExec normals shader is not compiled for this platform | Rebuild the plugin (`build_plugin.bat`) and let the shaders compile. Check the log for `RigExecNormals.usf` errors |

To see the fallback on a machine that runs the GPU path, set `RigExec.ForceCPUFallback 1` in the console and
reload the character (toggle **Draw On GPU**, or reopen the level). Set it back to 0 the same way.

The fallback draws correctly. It is only slower: each pose recomputes the normals and re-uploads the meshes on
the CPU.

## Load failures (`LogRigExec`)

| Message | Cause and fix |
| --- | --- |
| `cannot read <file>` | `RigExecFile` or `ControlsFile` does not exist. Relative paths resolve against the **project** directory, not the working directory |
| `cannot open the rig: unsupported .rigexec major version` | The file was baked with an older (or newer) container format than the vendored runtime reads (`RigExecBinaryVersion` in `include/rigExecBinary/container.h`). Re-bake: `prepare_biped.bat`, then `Examples\RigExecBiped\Setup_RigExecBiped.bat` |
| `cannot open the rig: <reason>` | The runtime refused the `.rigexec`. Usually the bake is from a different usdRig than the vendored runtime (format version, unknown opcode). Re-bake with the same usdRig the runtime was copied from (see `SOURCE.txt`), or update the runtime (see [building.md](building.md#updating-it)) |
| `cannot open <stage>` | `StageFile` is missing, or Unreal's USD cannot read it. The example expects `..\usdRig` beside this repository. Check `../../../usdRig/examples/biped/Biped_stack.usda` from the project folder |
| `no mesh in <stage> matches the rig's points` | The stage's mesh paths are not the ones the rig was baked with. Use the same stage (and the same `--rig`) that `rigExecBake` used |
| `<n> of <m> channels are not live` | Informational. Those channels were baked as constants. Bake with `--poseable` to make property-chain inputs live |
| `property chains replayed` | The bake was not `--poseable`, so face sliders and other chain-driven avars will not move anything |

## In the viewport

**Mesh is there but controls do nothing.**
- Check that `ControlsFile` is set, and that it is the JSON
  `build_control_rig.py` wrote element names into. A freshly exported file
  has no `"element"` fields. Re-run `build_control_rig.py`, then reload.
- Check that the `Controls` component's Control Rig Class is the asset made
  from that same JSON.

**No control gizmos.** Open the sequence, or call
`URigExecRuntimeLibrary::ShowControlRigInViewport`. Turn on **Controls As
Overlay** (`SetControlsAsOverlay(true)`), since body controls sit inside the
mesh. If the shapes have no mesh, the Control Rig's shape library was not
loaded: `ShowControlRigInViewport` loads it.

**Mesh looks inside out or mirrored.** The stage's `upAxis` or
`metersPerUnit` differs from what the controls file recorded. Re-export the
controls from the same stage.

**A limb pops when switching IK/FK.** You set the `ikfk` channel directly.
Use the picker's IK/FK button, or `SwitchLimb`, which matches the pose
first.

**Picker says there is no picker file.** Run `export_picker.py` and save the
result next to the controls file as `<Name>_picker.json`.

## Building

**`LNK2019` / `LNK2001` on `rigExec::` symbols.** The headers and libraries
in `RigExecLib` come from different builds. Re-run
`Tools\update_rigexec_lib.py` after building usdRig, so both are copied
together.

**`LNK2038` mismatch on `_ITERATOR_DEBUG_LEVEL` or `RuntimeLibrary`.** The
libraries were built in Debug or with the static CRT. Build usdRig in
Release with the default `/MD`.

**Unresolved STL symbols such as `__std_*`.** The libraries were built with
a newer MSVC than the one Unreal uses. Build usdRig with the same toolset or
an older one (see [building.md](building.md#toolchain-compatibility)).

**Paths too long.** Keep `RIGEXEC_PLUGIN_OUT` short (the default is
`<repo>\Build\RigExec`), or clone the repository closer to the drive root.

**"Missing modules, rebuild?" when opening the example.** The installed
plugin is missing or was built for a different engine version. Run
`build_plugin.bat` with `UE_ROOT` set to the engine you open the project
with.

## Rig preparation

**`ModuleNotFoundError: pxr` or `rigexec`.** Run the export scripts with
usdRig's Python, through `prepare_biped.bat` or after
`call ..\usdRig\bin\_env.bat` (then use `%PY%`).

**`ModuleNotFoundError: gizmoMath` / `pickerScene`.** The scripts cannot
find usdRig's usdview plug-in. Set `RIGEXEC_ROOT` to the usdRig checkout.
