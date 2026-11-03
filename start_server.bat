@echo off
title Mee-a-rai ROM Store - PC Server
cd /d "%~dp0"

echo ========================================================
echo        Mee-a-rai ROM Store - PC Wireless Bridge
echo ========================================================
echo.

where python >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] Python is not installed or not in PATH!
    echo Please install Python 3.8+ from https://www.python.org/
    echo Make sure to check "Add Python to PATH" during installation.
    echo.
    pause
    exit /b 1
)

python -c "import bottle" >nul 2>nul
if %errorlevel% neq 0 (
    echo [INFO] Installing required dependencies (bottle)...
    pip install -r requirements.txt
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to install dependencies. Please run 'pip install bottle'.
        pause
        exit /b 1
    )
)

echo Starting Web Dashboard & API Server...
echo Opening browser at http://localhost:8080 ...
echo.

start "" "http://localhost:8080"
python server.py

pause
