@echo off
chcp 65001 >nul
net session >nul 2>&1
if errorlevel 1 (
  echo Ce programme doit etre lance en administrateur.
  echo Clic droit sur Installer.bat, puis "Executer en tant qu'administrateur".
  pause
  exit /b 1
)
cd /d "%~dp0"
if not exist "thdatmos.dll" (
  echo ERREUR : thdatmos.dll doit etre dans le meme dossier que MpcAudioRendererAtmos.ax.
  pause
  exit /b 1
)
regsvr32 /s "%~dp0MpcAudioRendererAtmos.ax"
if errorlevel 1 (
  echo ERREUR : l'enregistrement du renderer a echoue.
  pause
  exit /b 1
)
echo.
echo MPC Audio Renderer Atmos est installe.
echo.
echo Dans MPC-BE : Options ^> Lecture ^> Sortie ^> Audio Renderer : choisir "MPC Audio Renderer Atmos".
echo Decodeur audio de MPC-BE : TrueHD coche, AC-3 / E-AC-3 / DTS / DTS-HD decoches.
echo Windows : sortie HDMI ^> Son spatial ^> "Dolby Atmos for home theater".
echo.
echo Ne deplacez plus ce dossier : pour le changer de place, desinstallez d'abord.
pause
