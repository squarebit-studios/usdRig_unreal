@echo off
rem Packages the SquarebitSubdivs plugin for this engine and installs it into
rem the example project, where the biped draws through its Dynamic Mesh
rem component (subdivided on the GPU). Optional: the project opens without it,
rem drawing the rig's own meshes.
rem
rem SQUAREBIT_SUBDIVS names the SquarebitSubdivs checkout (default:
rem ..\SquarebitSubdivs). UE_ROOT names the engine install; SUBDIVS_PLUGIN_OUT
rem the package folder, kept short because the intermediate paths are long.
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
if not defined SQUAREBIT_SUBDIVS (for %%I in ("%~dp0..\SquarebitSubdivs") do set "SQUAREBIT_SUBDIVS=%%~fI")
if not defined SUBDIVS_PLUGIN_OUT set "SUBDIVS_PLUGIN_OUT=%~dp0Build\SqbSubdivs"
set "UPLUGIN=%SQUAREBIT_SUBDIVS%\Plugins\SquarebitSubdivs\SquarebitSubdivs.uplugin"
if not exist "%UPLUGIN%" (
    echo no SquarebitSubdivs checkout at %SQUAREBIT_SUBDIVS%; set SQUAREBIT_SUBDIVS
    exit /b 1
)
call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
    -Plugin="%UPLUGIN%" -Package="%SUBDIVS_PLUGIN_OUT%" -TargetPlatforms=Win64 -Rocket
if errorlevel 1 exit /b 1
robocopy "%SUBDIVS_PLUGIN_OUT%" "%~dp0Examples\RigExecBiped\Plugins\SquarebitSubdivs" /MIR /XD Intermediate /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
echo installed into %~dp0Examples\RigExecBiped\Plugins\SquarebitSubdivs
exit /b 0
