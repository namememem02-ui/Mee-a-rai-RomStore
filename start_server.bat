@echo off
chcp 65001 >nul
setlocal
title Mee-a-rai ROM Store - PC Server
cd /d "%~dp0"

echo ========================================================
echo        Mee-a-rai ROM Store - PC Wireless Bridge
echo ========================================================
echo.

set "PY_CMD="
where python >nul 2>nul
if not errorlevel 1 (
    set "PY_CMD=python"
) else (
    where py >nul 2>nul
    if not errorlevel 1 (
        set "PY_CMD=py"
    )
)

if "%PY_CMD%"=="" goto NO_PYTHON

echo Starting Web Dashboard and Server...
echo Opening web browser at http://localhost:8080 ...
start "" "http://localhost:8080"
%PY_CMD% server.py
goto END

:NO_PYTHON
echo [ERROR] Python is not installed or not found in PATH!
echo Please install Python 3.8+ from https://www.python.org/
echo Make sure to check "Add Python to PATH" during installation.
echo.
pause
exit /b 1

:END
pause
