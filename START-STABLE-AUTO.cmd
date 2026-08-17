@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\windows\Set-Modem-Mode.ps1" -Mode auto
if errorlevel 1 pause & exit /b 1
call START-V92ISP-WINDOWS.cmd
