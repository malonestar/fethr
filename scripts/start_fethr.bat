@echo off
REM Start fethr with no console window.
REM Prefers a .venv next to the repo, then whatever pythonw is on PATH.

setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

if exist ".venv\Scripts\pythonw.exe" (
    start "" ".venv\Scripts\pythonw.exe" -m fethr %*
) else (
    start "" pythonw.exe -m fethr %*
)

popd
endlocal
