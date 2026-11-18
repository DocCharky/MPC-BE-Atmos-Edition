@echo off
chcp 65001 >nul
net session >nul 2>&1
if errorlevel 1 (
  echo Ce programme doit etre lance en administrateur.
  echo Clic droit sur Desinstaller.bat, puis "Executer en tant qu'administrateur".
  pause
  exit /b 1
)
regsvr32 /s /u "%~dp0MpcAudioRendererAtmos.ax"
echo MPC Audio Renderer Atmos est desinstalle.
pause
