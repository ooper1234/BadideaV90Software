@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\windows\Setup-And-Start.ps1"
if errorlevel 1 pause
