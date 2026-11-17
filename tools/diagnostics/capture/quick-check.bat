@echo off
chcp 65001 >nul
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 goto trypy
python quick-check.py
goto done
:trypy
where py >nul 2>nul
if errorlevel 1 goto nopython
py quick-check.py
goto done
:nopython
echo [!] Python not found. Run manually: python quick-check.py
:done
echo.
pause
