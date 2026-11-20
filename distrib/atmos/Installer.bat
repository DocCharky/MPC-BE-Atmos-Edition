@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo This script must be run as administrator.
  echo Right-click Installer.bat, then "Run as administrator".
  pause
  exit /b 1
)
cd /d "%~dp0"
if not exist "thdatmos.dll" (
  echo ERROR: thdatmos.dll must be in the same folder as MpcSpatialClock.ax.
  pause
  exit /b 1
)
regsvr32 /s "%~dp0MpcSpatialClock.ax"
if errorlevel 1 (
  echo ERROR: registering the renderer failed.
  pause
  exit /b 1
)
echo.
echo MPC SpatialClock is installed.
echo.
echo MPC-BE: Options ^> Audio ^> Audio renderer: select "MPC SpatialClock (external)".
echo MPC-BE audio decoder: TrueHD and E-AC-3 pass-through ON, DTS and AC-3 OFF.
echo Windows: HDMI output ^> Spatial sound ^> "Dolby Atmos for home theater".
echo.
echo Do not move this folder: to move it, uninstall first.
pause
