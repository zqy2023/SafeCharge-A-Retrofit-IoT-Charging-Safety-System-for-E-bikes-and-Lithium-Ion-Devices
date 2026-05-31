"""
SafeCharge Web Dashboard  –  Flask + Socket.IO backend
  POST /api/data       ← ESP32 pushes sensor readings (JSON, every ~3 s)
  GET  /api/command    ← ESP32 polls after each POST for pending commands
  GET  /api/latest     ← most recent reading
  GET  /api/history    ← historical data  (?range=1h|6h|24h|7d)
  GET  /api/events     ← alert/event log  (?limit=N)
  GET/POST /api/thresholds
  POST /api/reset      ← browser requests relay reset
"""

from typing import Optional
from flask import Flask, request, jsonify, render_template
from flask_socketio import SocketIO
from flask_cors import CORS
import database as db

app = Flask(__name__)
CORS(app)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

_prev_state: str = "Normal"
_pending_command: Optional[str] = None
_pending_thresholds: Optional[dict] = None


# ── Frontend ───────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


# ── ESP32 Data Ingestion ───────────────────────────────────────────────────────

@app.route("/api/data", methods=["POST"])
def receive_data():
    global _prev_state
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"error": "no json"}), 400

    db.insert_reading(data)
    _detect_events(data.get("state", "Normal"), data)
    _prev_state = data.get("state", "Normal")

    socketio.emit("sensor_update", data)
    return jsonify({"status": "ok"})


# ── ESP32 Command Polling ──────────────────────────────────────────────────────

@app.route("/api/command")
def get_command():
    global _pending_command, _pending_thresholds
    # Threshold updates take priority so they reach the ESP32 on the next poll
    if _pending_thresholds is not None:
        resp = {"command": "SET_THRESHOLDS", **_pending_thresholds}
        _pending_thresholds = None
        return jsonify(resp)
    cmd, _pending_command = _pending_command, None
    return jsonify({"command": cmd})


# ── REST ───────────────────────────────────────────────────────────────────────

@app.route("/api/latest")
def get_latest():
    return jsonify(db.get_latest() or {})


@app.route("/api/history")
def get_history():
    return jsonify(db.get_history(request.args.get("range", "1h")))


@app.route("/api/events")
def get_events():
    return jsonify(db.get_events(int(request.args.get("limit", 100))))


@app.route("/api/thresholds", methods=["GET", "POST"])
def thresholds():
    global _pending_thresholds
    if request.method == "GET":
        return jsonify(db.get_thresholds() or {})
    body = request.get_json(silent=True) or {}
    cw  = body.get("current_warn",   2.5)
    cf  = body.get("current_fault",  3.5)
    tw  = body.get("temp_warn",     38.0)
    tf  = body.get("temp_fault",    45.0)
    dvw = body.get("dc_volt_warn",  13.5)
    dvf = body.get("dc_volt_fault", 14.8)
    dpw = body.get("dc_power_warn", 25.0)
    dpf = body.get("dc_power_fault",40.0)
    db.update_thresholds(cw, cf, tw, tf, dvw, dvf, dpw, dpf)
    # Queue a command so the ESP32 picks it up on the next /api/command poll
    _pending_thresholds = {
        "current_warn":   cw,  "current_fault":  cf,
        "temp_warn":      tw,  "temp_fault":     tf,
        "dc_volt_warn":   dvw, "dc_volt_fault":  dvf,
        "dc_power_warn":  dpw, "dc_power_fault": dpf,
    }
    socketio.emit("thresholds_updated", _pending_thresholds)
    return jsonify({"status": "ok"})


@app.route("/api/reset", methods=["POST"])
def reset():
    global _pending_command
    _pending_command = "RESET"
    db.insert_event("INFO", "Manual reset requested from dashboard", {}, "RESET")
    socketio.emit("command_sent", {"command": "RESET"})
    return jsonify({"status": "ok"})


@app.route("/api/relay/off", methods=["POST"])
def relay_off():
    global _pending_command
    _pending_command = "CUT"
    db.insert_event("INFO", "Manual power cut requested from dashboard", {}, "RELAY_OFF")
    socketio.emit("command_sent", {"command": "CUT"})
    return jsonify({"status": "ok"})


# ── Event Detection (state transition only) ───────────────────────────────────

def _detect_events(state: str, data: dict):
    if state == _prev_state:
        return
    mapping = {
        "Warning": ("WARNING",  "System entered WARNING state",              "BUZZER_ON"),
        "Fault":   ("FAULT",    "FAULT detected – relay disconnected",       "RELAY_OFF"),
        "Latched": ("FAULT",    "System LATCHED – manual reset required",    "RELAY_OFF"),
        "Normal":  ("RECOVERY", "System returned to NORMAL",                 "RELAY_ON"),
    }
    entry = mapping.get(state)
    if entry:
        db.insert_event(entry[0], entry[1], data, entry[2])


# ── Entry Point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    db.init_db()
    print("\n  SafeCharge Dashboard  →  http://localhost:5050\n")
    socketio.run(app, host="0.0.0.0", port=5050, debug=False, use_reloader=False)
