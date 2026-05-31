#!/bin/bash
echo ""
echo " SafeCharge Dashboard - Mac / Linux"
echo " ===================================="
echo ""

PYTHON=$(command -v python3 2>/dev/null || command -v python 2>/dev/null)
if [ -z "$PYTHON" ]; then
    echo " ERROR: Python not found."
    echo " Mac:   brew install python"
    echo " Linux: sudo apt install python3"
    exit 1
fi

echo " [1/3] Installing Python packages..."
$PYTHON -m pip install -r requirements.txt -q

echo " [2/3] Downloading static assets (first run only)..."
$PYTHON setup_static.py

echo " [3/3] Starting server..."
echo ""
echo " Open in browser:  http://localhost:5000"
echo " Press Ctrl+C to stop."
echo ""
$PYTHON app.py
