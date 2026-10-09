@echo off
rem Opens the RigExec biped project in Unreal Engine 5.8. UE_ROOT names the
rem engine install (default: the launcher's location for 5.8).
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
start "" "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%~dp0RigExecBiped.uproject"
