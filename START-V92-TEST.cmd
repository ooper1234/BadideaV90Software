@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\windows\Set-Modem-Mode.ps1" -Mode v92
if errorlevel 1 pause & exit /b 1
rem IMPORTANT: build the current source before every V.92/V.90 interoperability test.
rem Older packages carried many staged *-fixed.exe files; launching one of those
rem made a new source patch appear ineffective even though it had never run.
call START-V92ISP-WINDOWS.cmd
if errorlevel 1 pause
