@echo off
title SafeCharge Dashboard
echo.
echo  SafeCharge Dashboard - Windows
echo  ================================
echo.

python --version >nul 2>&1
if errorlevel 1 (
    echo  ERROR: Python not found.
    echo  Install from https://python.org  then re-run this script.
    pause & exit /b 1
)

echo  [1/3] Installing Python packages...
python -m pip install -r requirements.txt -q

echo  [2/3] Downloading static assets (first run only)...
python setup_static.py

echo  [3/3] Starting server...
echo.
echo  Open in browser:  http://localhost:5000
echo  Press Ctrl+C to stop.
echo.
python app.py
pause
