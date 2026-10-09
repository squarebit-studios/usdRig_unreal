@echo off
rem Rebuilds the example project's Control Rig, level and sequence from its
rem RigExecData by running Scripts\setup_example_project.py once as a project
rem startup script (passed on the command line, so no config is changed).
rem The editor opens, builds and saves the content, then quits; the log
rem (Saved\Logs\RigExecBiped.log) says RIGEXEC_SETUP_OK or
rem RIGEXEC_SETUP_FAILED. Build the plugin first (build_plugin.bat) and close
rem any editor open on the project. UE_ROOT names the engine install.
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
for %%I in ("%~dp0..\..\Scripts\setup_example_project.py") do set "SETUP=%%~fI"
set "SETUP=%SETUP:\=/%"
rem The level and sequence are rebuilt from scratch; removing them first
rem keeps the editor from opening (and so locking) the old level.
del /q "%~dp0Content\RigExec\L_RigExec.umap" "%~dp0Content\RigExec\LS_RigExec.uasset" 2>nul
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%~dp0RigExecBiped.uproject" ^
    "-ini:Engine:[/Script/PythonScriptPlugin.PythonScriptPluginSettings]:StartupScripts=%SETUP%" ^
    -log -unattended
findstr /C:"RIGEXEC_SETUP_OK" "%~dp0Saved\Logs\RigExecBiped.log" >nul || (
    echo setup failed; see %~dp0Saved\Logs\RigExecBiped.log
    exit /b 1
)
echo example project rebuilt
exit /b 0
