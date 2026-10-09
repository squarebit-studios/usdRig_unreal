@echo off
rem Packages the RigExec plugin and installs it into the example project.
rem
rem Needs nothing from usdRig: the runtime libraries are vendored under
rem Plugins\RigExec\Source\ThirdParty\RigExecLib. UE_ROOT names the engine
rem install (default: the launcher's location for 5.8); RIGEXEC_PLUGIN_OUT the
rem package folder, kept short because the intermediate paths are long.
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
if not defined RIGEXEC_PLUGIN_OUT set "RIGEXEC_PLUGIN_OUT=%~dp0Build\RigExec"
if not exist "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" (
    echo no Unreal Engine at %UE_ROOT%; set UE_ROOT
    exit /b 1
)
call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
    -Plugin="%~dp0Plugins\RigExec\RigExec.uplugin" -Package="%RIGEXEC_PLUGIN_OUT%" ^
    -TargetPlatforms=Win64 -Rocket
if errorlevel 1 exit /b 1
robocopy "%RIGEXEC_PLUGIN_OUT%" "%~dp0Examples\RigExecBiped\Plugins\RigExec" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
echo installed into %~dp0Examples\RigExecBiped\Plugins\RigExec
exit /b 0
