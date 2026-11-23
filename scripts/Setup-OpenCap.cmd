@echo off
setlocal
set "action=Install"
if /I "%~1"=="Remove" set "action=Remove"
if not "%~1"=="" if /I not "%~1"=="Install" if /I not "%~1"=="Remove" (
    echo Usage: Setup-OpenCap.cmd [Install^|Remove]
    exit /b 2
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-OpenCap.ps1" -Action %action%
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
