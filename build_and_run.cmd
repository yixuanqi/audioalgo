@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_and_run_editor.ps1" %*
set "audioalgo_exit=%errorlevel%"
if not "%audioalgo_exit%"=="0" (
    echo.
    echo AudioAlgo build or launch failed. See the error above.
    if not defined AUDIOALGO_NO_PAUSE pause
)
exit /b %audioalgo_exit%
