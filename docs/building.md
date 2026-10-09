# Building

## What you need

| | |
| --- | --- |
| Engine | Unreal Engine 5.8 (launcher install). Set `UE_ROOT` if it is not at `C:\Program Files\Epic Games\UE_5.8` |
| Compiler | Visual Studio 2022, "Game development with C++" workload (the toolchain UE 5.8 requires) |
| Platform | Win64 |
| usdRig | **Not** needed to build. The runtime is vendored under `Plugins/RigExec/Source/ThirdParty/RigExecLib` |

## Build and install into the example

```bat
build_plugin.bat
```

This runs Unreal's automation tool:

```
RunUAT BuildPlugin -Plugin=Plugins\RigExec\RigExec.uplugin -Package=Build\RigExec -TargetPlatforms=Win64 -Rocket
```

It then mirrors the packaged plugin into
`Examples\RigExecBiped\Plugins\RigExec`. Both `Build\` and the example's
`Plugins\` folder are build output and are git-ignored.

| Variable | Default | Meaning |
| --- | --- | --- |
| `UE_ROOT` | `C:\Program Files\Epic Games\UE_5.8` | Engine install |
| `RIGEXEC_PLUGIN_OUT` | `<repo>\Build\RigExec` | Package folder. Keep it short: UBT's intermediate paths under it get long, and Windows' 260-character limit fails the build |

BuildPlugin compiles the editor and game targets, so a successful run means
the plugin compiles and links outside any project.

## Use the plugin in another project

1. Run `build_plugin.bat`.
2. Copy `Build\RigExec` into `<YourProject>\Plugins\RigExec`.
3. Enable **RigExec** in the project. This also enables USDCore and
   ControlRig, which the plugin lists as dependencies. Python Editor Script
   Plugin is needed only to run the scripts in `Scripts\`.

If the project has its own C++ code you can instead copy `Plugins\RigExec`
(the source) into it, and the project's build compiles the plugin.

## The vendored runtime

`Plugins/RigExec/Source/ThirdParty/RigExecLib` is an External module:

```
RigExecLib/
├── RigExecLib.Build.cs     adds include/ as a system include path and links the two libs
├── include/
│   ├── rigExecRuntime/     runtime.h, runtimeMath.h, store.h, values.h
│   ├── rigExecBinary/      the .rigexec section readers runtime.h reaches
│   └── rigExecMath/        header-only kernels the runtime's headers include
├── lib/Win64/
│   ├── rigExecRuntime.lib
│   └── rigExecBinary.lib
└── SOURCE.txt              usdRig commit, uncommitted changes, the files copied
```

Only the headers that `rigExecRuntime/runtime.h` reaches through its
includes are copied. The libraries are compiled code, so their private
headers and kernels stay behind.

### Updating it

When usdRig's runtime changes (new opcodes, a new `.rigexec` version, a fix):

```bat
rem in ..\usdRig
bin\build_rigexec.bat

rem back here
python Tools\update_rigexec_lib.py          & rem or: ... <path to usdRig>
build_plugin.bat
```

`update_rigexec_lib.py` takes the usdRig root as an argument, else
`RIGEXEC_ROOT`, else `..\usdRig`. It reads the libraries from
`<root>\build`, or from `RIGEXEC_BUILD_DIR` if that is set. It deletes the
old copy first, so headers the runtime no longer uses do not linger. It
records the commit, plus any uncommitted changes under `libs/`, in
`SOURCE.txt`. Commit the result together with any plugin change it needs.

**Keep headers and libraries from the same build.** The script copies both
in one go. It refuses to copy while any runtime source, or any header it
would copy, is newer than the libraries, and lists those files. Rebuild
usdRig, or pass `--force` if you know the change does not matter. Do not hand-copy one without the other: `runtime.h` describes the
class layout the libraries were compiled with.

**Re-bake after a format change.** A `.rigexec` written by an older
`rigExecBake` can be refused by a newer runtime, and the reverse. The
reader's error says which section failed. Run `prepare_biped.bat` (or your
own bake) after updating.

### Toolchain compatibility

The libraries come from usdRig's CMake build: MSVC (`cl` 14.38, VS 2022),
Ninja, `Release`, dynamic CRT (`/MD`), C++17, `/fp:precise`. These match
what Unreal links against for Development and Shipping editor targets, so no
special flags are needed.

- Unreal links the release CRT in every configuration, Debug included,
  unless an engine source build sets `bDebugBuildsActuallyUseDebugCRT`. So
  the Release libraries link in all of them.
- `/fp:precise` matters. The runtime keeps the baked evaluator's
  floating-point operation order so its results match usdview. This is why
  the runtime is linked prebuilt rather than compiled by UBT with Unreal's
  flags.
- A much newer MSVC in usdRig than in Unreal can produce `LNK2019`/`LNK2001`
  on newer STL internals. Build usdRig with the same or an older toolset
  than Unreal's.

## Compiling during development

To iterate on the plugin's C++ without packaging each time, put the plugin
into a C++ project:

1. Create a blank C++ project in UE 5.8, or add a dummy C++ class to the
   example project.
2. Copy or junction `Plugins\RigExec` into that project's `Plugins\` folder:
   `mklink /J <Project>\Plugins\RigExec <repo>\Plugins\RigExec`.
3. Generate project files and build `<Project>Editor` (Development Editor)
   from Visual Studio or Rider. Live Coding works for `.cpp` changes.
