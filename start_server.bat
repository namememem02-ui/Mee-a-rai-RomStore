@echo off
setlocal
title Mee-a-rai ROM Store - PC Server
cd /d "%~dp0"

echo ========================================================
echo        Mee-a-rai ROM Store - PC Wireless Bridge
echo ========================================================
echo.

where python >nul 2>nul
if errorlevel 1 goto NO_PYTHON

python -c "import bottle" >nul 2>nul
if errorlevel 1 goto INSTALL_DEPS
goto START_APP

:INSTALL_DEPS
echo [INFO] Installing required bottle library...
pip install -r requirements.txt
if errorlevel 1 goto FAIL_DEPS
goto START_APP

:START_APP
echo Starting Web Dashboard and Server...
echo Opening web browser at http://localhost:8080 ...
start "" "http://localhost:8080"
python server.py
goto END

:NO_PYTHON
echo [ERROR] Python is not installed or not found in PATH!
echo Please install Python 3.8+ from https://www.python.org/
echo Make sure to check "Add Python to PATH" during installation.
echo.
pause
exit /b 1

:FAIL_DEPS
echo [ERROR] Failed to install dependencies.
echo Please run: pip install bottle
echo.
pause
exit /b 1

:END
pause
