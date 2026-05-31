import sqlite3
import os
from datetime import datetime, timedelta

DB_PATH = os.path.join(os.path.dirname(__file__), "safecharge.db")

# ── Schema ─────────────────────────────────────────────────────────────────────

def now_local_iso():
    return datetime.now().astimezone().replace(microsecond=0).isoformat()

def init_db():
    with _conn() as c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     DATETIME DEFAULT CURRENT_TIMESTAMP,
            current_a     REAL,
            temperature_c REAL,
            humidity_pct  REAL,
            smoke         INTEGER,
            smoke_detected INTEGER,
            relay_on      INTEGER,
            dc_voltage_v  REAL,
            dc_current_a  REAL,
            dc_power_w    REAL,
            ina219_ok     INTEGER,
            state         TEXT
        );

        CREATE TABLE IF NOT EXISTS events (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp   DATETIME DEFAULT CURRENT_TIMESTAMP,
            event_type  TEXT,
            description TEXT,
            current_a   REAL,
            temperature_c REAL,
            smoke_detected INTEGER,
            action      TEXT
        );

        CREATE TABLE IF NOT EXISTS thresholds (
            id              INTEGER PRIMARY KEY CHECK (id = 1),
            current_warn    REAL DEFAULT 2.5,
            current_fault   REAL DEFAULT 3.5,
            temp_warn       REAL DEFAULT 38.0,
            temp_fault      REAL DEFAULT 45.0,
            dc_volt_warn    REAL DEFAULT 13.5,
            dc_volt_fault   REAL DEFAULT 14.8,
            dc_power_warn   REAL DEFAULT 25.0,
            dc_power_fault  REAL DEFAULT 40.0,
            updated_at      DATETIME DEFAULT CURRENT_TIMESTAMP
        );

        INSERT OR IGNORE INTO thresholds (id) VALUES (1);
        """)

        # Migrate existing databases that predate INA219 / smoke-ADC support
        _add_col(c, "sensor_readings", "dc_voltage_v  REAL")
        _add_col(c, "sensor_readings", "dc_current_a  REAL")
        _add_col(c, "sensor_readings", "dc_power_w    REAL")
        _add_col(c, "sensor_readings", "ina219_ok     INTEGER")
        _add_col(c, "sensor_readings", "smoke         INTEGER")
        _add_col(c, "thresholds",      "dc_volt_warn   REAL DEFAULT 13.5")
        _add_col(c, "thresholds",      "dc_volt_fault  REAL DEFAULT 14.8")
        _add_col(c, "thresholds",      "dc_power_warn  REAL DEFAULT 25.0")
        _add_col(c, "thresholds",      "dc_power_fault REAL DEFAULT 40.0")


def _add_col(conn, table, col_def):
    """ALTER TABLE … ADD COLUMN – silently ignored if the column already exists."""
    try:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {col_def}")
    except Exception:
        pass


# ── Helpers ────────────────────────────────────────────────────────────────────

def _conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def _row_to_dict(row):
    return dict(row) if row else None

# ── Writes ─────────────────────────────────────────────────────────────────────

def insert_reading(data: dict):
    with _conn() as c:
        c.execute("""
            INSERT INTO sensor_readings
                (timestamp, current_a, temperature_c, humidity_pct,
                 smoke, smoke_detected, relay_on,
                 dc_voltage_v, dc_current_a, dc_power_w, ina219_ok,
                 state)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            now_local_iso(),
            data.get("current_a"),
            data.get("temperature_c"),
            data.get("humidity_pct"),
            data.get("smoke"),
            1 if data.get("smoke_detected") else 0,
            1 if data.get("relay_on") else 0,
            data.get("dc_voltage_v"),
            data.get("dc_current_a"),
            data.get("dc_power_w"),
            1 if data.get("ina219_ok") else 0,
            data.get("state", "Normal"),
        ))

def insert_event(event_type: str, description: str, data: dict, action: str = ""):
    with _conn() as c:
        c.execute("""
            INSERT INTO events
                (timestamp, event_type, description, current_a,
                 temperature_c, smoke_detected, action)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (
            now_local_iso(),
            event_type,
            description,
            data.get("current_a"),
            data.get("temperature_c"),
            1 if data.get("smoke_detected") else 0,
            action,
        ))

# ── Reads ──────────────────────────────────────────────────────────────────────

def get_latest():
    with _conn() as c:
        row = c.execute(
            "SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1"
        ).fetchone()
    return _row_to_dict(row)

def get_history(range_str: str = "1h", max_points: int = 200):
    delta_map = {"1h": 1, "6h": 6, "24h": 24, "7d": 168}
    hours = delta_map.get(range_str, 1)

    since = (datetime.now().astimezone() - timedelta(hours=hours)).replace(microsecond=0).isoformat()

    with _conn() as c:
        rows = c.execute("""
            SELECT timestamp, current_a, temperature_c,
                   humidity_pct, smoke, smoke_detected,
                   dc_voltage_v, dc_current_a, dc_power_w, ina219_ok,
                   state
            FROM sensor_readings
            WHERE timestamp >= ?
            ORDER BY id ASC
        """, (since,)).fetchall()

    data = [dict(r) for r in rows]

    if len(data) > max_points:
        step = len(data) // max_points
        data = data[::step]

    return data

def get_events(limit: int = 50):
    with _conn() as c:
        rows = c.execute(
            "SELECT * FROM events ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in rows]

def get_thresholds():
    with _conn() as c:
        row = c.execute("SELECT * FROM thresholds WHERE id = 1").fetchone()
    return _row_to_dict(row)

def update_thresholds(current_warn, current_fault, temp_warn, temp_fault,
                      dc_volt_warn, dc_volt_fault, dc_power_warn, dc_power_fault):
    with _conn() as c:
        c.execute("""
            UPDATE thresholds
            SET current_warn=?, current_fault=?,
                temp_warn=?, temp_fault=?,
                dc_volt_warn=?, dc_volt_fault=?,
                dc_power_warn=?, dc_power_fault=?,
                updated_at=?
            WHERE id = 1
        """, (current_warn, current_fault, temp_warn, temp_fault,
              dc_volt_warn, dc_volt_fault, dc_power_warn, dc_power_fault,
              now_local_iso()))
