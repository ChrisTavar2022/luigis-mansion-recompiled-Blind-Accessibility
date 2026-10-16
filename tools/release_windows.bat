@echo off
rem Double-click to build, package and publish the Windows release (tools\release_windows.ps1).
cd /d "%~dp0.."
git pull --ff-only
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release_windows.ps1"
echo.
pause
