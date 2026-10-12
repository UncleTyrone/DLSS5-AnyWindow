@echo off
title DLSS5 AnyWindow package
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Package-Release.ps1" %*
if errorlevel 1 (
    echo PACKAGE FAILED
    exit /b 1
)
echo PACKAGE SUCCEEDED
