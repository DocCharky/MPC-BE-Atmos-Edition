@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo This script must be run as administrator.
  echo Right-click Uninstaller.bat, then "Run as administrator".
  pause
  exit /b 1
)
regsvr32 /s /u "%~dp0MpcSpatialClock.ax"
echo MPC SpatialClock is uninstalled.
pause
