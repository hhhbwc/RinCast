@echo off
setlocal enabledelayedexpansion
chcp 437 >nul

set "ROOT=%~dp0..\..\.."
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%i"
set "OUT=%ROOT%\CAPTURE-DATA\%STAMP%"
mkdir "%OUT%" 2>nul

echo ================================================
echo  RinCast test data collector
echo  Output: %OUT%
echo ================================================
echo.

echo [1/4] Copying PC logs...
if exist "%LOCALAPPDATA%\RinCast\logs\streamer.log" (
  copy /y "%LOCALAPPDATA%\RinCast\logs\streamer.log" "%OUT%\pc-streamer.log" >nul
  echo    OK pc-streamer.log
) else (
  echo    WARNING: no streamer.log found
)
if exist "%LOCALAPPDATA%\RinCast\logs\streamer.previous.log" (
  copy /y "%LOCALAPPDATA%\RinCast\logs\streamer.previous.log" "%OUT%\pc-streamer-previous.log" >nul
  echo    OK pc-streamer-previous.log
)

echo.
echo [2/4] Looking for adb device...
set "DEVID="
for /f "skip=1 tokens=1,2" %%a in ('adb devices') do (
  if "%%b"=="device" set "DEVID=%%a"
)
if "%DEVID%"=="" (
  echo    ERROR: no adb device. Connect headset / run: adb connect ^<ip:port^>
  echo.
  pause
  exit /b 1
)
echo    device = %DEVID%

echo.
echo [3/4] Dumping headset logcat ^(may take 10-30s^)...
adb -s %DEVID% logcat -d > "%OUT%\hmd-logcat.txt" 2>&1
echo    saved hmd-logcat.txt

echo.
echo [4/4] Writing info.txt...
(
  echo capture time : %STAMP%
  echo adb device   : %DEVID%
  echo pc log       : %LOCALAPPDATA%\RinCast\logs\streamer.log
) > "%OUT%\info.txt"

echo.
echo ================================================
echo  DONE. Folder:
echo  %OUT%
echo.
dir /b "%OUT%"
echo ================================================
echo.
echo Next: send that folder path to the AI.
pause
