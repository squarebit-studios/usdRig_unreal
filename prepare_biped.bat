@echo off
rem Rebuilds the example project's rig data from usdRig's
rem examples\biped\Biped_stack.usda: the poseable .rigexec, the controls
rem description and the picker layout. The project's Control Rig, level and
rem sequence are built from the controls file, so run
rem Scripts\setup_example_project.py in the editor after this.
rem
rem Runs under usdRig's own environment (its USD build, Python and
rem rigExecBake). RIGEXEC_ROOT names the usdRig checkout (default: ..\usdRig).
setlocal
if not defined RIGEXEC_ROOT (for %%I in ("%~dp0..\usdRig") do set "RIGEXEC_ROOT=%%~fI")
if not exist "%RIGEXEC_ROOT%\bin\_env.bat" (
    echo no usdRig checkout at %RIGEXEC_ROOT%; set RIGEXEC_ROOT
    exit /b 1
)
call "%RIGEXEC_ROOT%\bin\_env.bat"
set "STAGE=%RIG%\examples\biped\Biped_stack.usda"
set "DATA=%~dp0Examples\RigExecBiped\RigExecData"
"%RIG%\build\rigExecBake.exe" "%STAGE%" --rig /Biped/Rig --frames 1 --poseable -o "%DATA%\Biped.rigexec" || exit /b 1
"%PY%" "%~dp0Scripts\export_controls.py" "%STAGE%" "%DATA%\Biped_controls.json" || exit /b 1
"%PY%" "%~dp0Scripts\export_picker.py" "%STAGE%" "%DATA%\Biped_picker.json" || exit /b 1
exit /b 0
