# usdRig_unreal

**Documentation: <https://squarebit-studios.github.io/usdRig_unreal/>**

The RigExec plugin for Unreal Engine 5.8. It poses a [usdRig](../usdRig)
(RigExec) character inside the Unreal Editor and animates it with Control Rig
and Sequencer.

The rig itself is not rebuilt in Unreal. It is baked once, outside Unreal,
into a `.rigexec` file. The plugin evaluates that file with RigExec's binary
runtime, a USD-free C++17 library that is linked into the plugin as static
libraries. The editor's own USD build reads each mesh's topology and materials
from the source stage once, at load time. After that, every change to a
control re-poses the rig and deforms the meshes in place.

```
 usdRig (outside Unreal)                     Unreal Editor (this repo)
 ------------------------                    ----------------------------------------
 Biped_stack.usda --rigExecBake--> Biped.rigexec ----> RigExecRuntime (binary runtime)
                  --export_controls.py--> Biped_controls.json --> Control Rig asset + control map
                  --export_picker.py---> Biped_picker.json  ----> RigExec Picker tab
 Biped_stack.usda (meshes, materials) --------------------------> Dynamic meshes
```

## What's in this repository

| Path | What it is |
| --- | --- |
| `Plugins/RigExec/` | The plugin: `RigExecRuntime` (runtime module), `RigExecEditor` (the docked and hover pickers, Touch Pose) and `ThirdParty/RigExecLib` (the vendored RigExec runtime libraries and headers) |
| `Examples/RigExecBiped/` | A ready-to-open example project: the biped on a lit floor, its Control Rig, and a Level Sequence |
| `Scripts/` | Python: rig preparation (runs under usdRig's Python) and editor setup (runs inside Unreal) |
| `Tools/make_materials.py` | Regenerates the plugin's surface material (`M_RigExecSurface`) |
| `Tools/update_rigexec_lib.py` | Refreshes the vendored runtime from a built usdRig checkout |
| `build_plugin.bat` | Packages the plugin with Unreal's automation tool and installs it into the example project |
| `build_subdivs.bat` | For local testing with [Squarebit Subdivs](https://www.squarebitstudios.com/squarebit-subdivs): packages `../SquarebitSubdivs` into the example project |
| `prepare_biped.bat` | Rebuilds the example's rig data from usdRig's biped |
| `docs/` | Full documentation (see below) |

USD files are not copied here. The plugin reads meshes from the stage the rig
was baked from, which lives in the usdRig checkout.

## Requirements

- Windows 64-bit and Unreal Engine **5.8**. The default install location is
  `C:\Program Files\Epic Games\UE_5.8`; set `UE_ROOT` to override it.
- Visual Studio 2022 with the C++ workload, so Unreal can compile the plugin.
- A **usdRig checkout beside this repository** (`..\usdRig`) for:
  - the meshes: the example reads `..\usdRig\examples\biped\Biped_stack.usda`
  - rig preparation: `rigExecBake`, `export_controls.py` and
    `export_picker.py` use usdRig's USD build and Python
  - refreshing the vendored runtime

  Building the plugin needs nothing from usdRig.

## Quick start

```bat
build_plugin.bat
build_subdivs.bat            & rem optional: GPU subdivision (needs ..\SquarebitSubdivs)
Examples\RigExecBiped\Launch_RigExecBiped.bat
```

The project opens on `L_RigExec`, which has the biped, a floor and lights. To
pose the biped:

1. Open `LS_RigExec` from the Content Browser. It has a Control Rig track for
   the character.
2. Select controls in the viewport, or open **Tools > RigExec > Picker**. You
   can also press **P** over the viewport for the hover picker, or turn on
   **Tools > RigExec > Touch Pose** and click the character's skin.
3. Move a control. The rig evaluates and the meshes follow.
4. Key it like any Control Rig.

## Documentation

| Doc | Read it for |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | How the plugin works: modules, data flow, evaluation, coordinate conversion |
| [docs/building.md](docs/building.md) | Building and packaging, the vendored runtime, using the plugin in another project |
| [docs/rig-preparation.md](docs/rig-preparation.md) | Baking a rig and exporting its controls and picker, plus the file formats |
| [docs/editor-workflow.md](docs/editor-workflow.md) | Setting up a character, animating in Sequencer, the picker, IK/FK and space switching |
| [docs/subdivision.md](docs/subdivision.md) | Drawing the character subdivided on the GPU with Squarebit Subdivs, and how Touch Pose reaches it |
| [docs/api.md](docs/api.md) | C++, Blueprint and Python API reference |
| [docs/plans/touchpose-remaining.md](docs/plans/touchpose-remaining.md) | Touch Pose work still to do (on hold) |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Known limits and common failures |

## Limits at a glance

- **Drawn on the GPU by default.** Each pose uploads the points once, and normals are computed on the GPU. There
  is a CPU fallback, which the component, a toast and the log all report. The GPU skin writes motion vectors
  for TSR and motion blur, and is refit for hardware ray tracing in projects that enable it.

- **Editor only.** Unreal builds USD into the editor, not into packaged games,
  and the meshes are read from the stage.
- **Win64 only.** The vendored runtime libraries are MSVC builds.
- **No UVs or textures.** Materials carry their preview-surface colour,
  roughness and metallic values. A procedural shader, such as the eye's iris,
  falls back to a neutral grey.
- **Some channels are dropped.** A channel the binary runtime cannot take
  (such as a mode switch that is read at bake time) is dropped at load and
  logged.
