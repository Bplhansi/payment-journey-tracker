"""SQLite storage for engine records. Every write is keyed by ID, so applying the same
record twice (which happens when the engine replays events after a crash) changes nothing."""

from __future__ import annotations

import json
import sqlite3
from collections.abc import Iterable
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS payments (
    payment_id     TEXT PRIMARY KEY,
    stage          TEXT NOT NULL,
    amount_minor   INTEGER NOT NULL,
    currency       TEXT NOT NULL,
    last_update_ms INTEGER NOT NULL,
    invalid        INTEGER NOT NULL,
    finished       INTEGER NOT NULL,
    events_applied INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    event_id     TEXT PRIMARY KEY,
    payment_id   TEXT NOT NULL,
    stage        TEXT NOT NULL,
    timestamp_ms INTEGER NOT NULL,
    amount_minor INTEGER NOT NULL,
    currency     TEXT NOT NULL,
    status       TEXT NOT NULL,
    seen_at_ms   INTEGER NOT NULL,
    duplicated   INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS events_by_payment ON events (payment_id, timestamp_ms);
CREATE TABLE IF NOT EXISTS alerts (
    alert_id      TEXT PRIMARY KEY,
    alert_type    TEXT NOT NULL,
    severity      TEXT NOT NULL,
    payment_id    TEXT,
    stage         TEXT,
    raised_at_ms  INTEGER NOT NULL,
    detail        TEXT NOT NULL,
    history_json  TEXT NOT NULL,
    status        TEXT NOT NULL,
    cleared_at_ms INTEGER
);
CREATE INDEX IF NOT EXISTS alerts_by_payment ON alerts (payment_id);
CREATE INDEX IF NOT EXISTS alerts_by_status ON alerts (status, raised_at_ms);
CREATE TABLE IF NOT EXISTS metrics (
    wall_ms             INTEGER PRIMARY KEY,
    events_total        INTEGER NOT NULL,
    events_per_sec      REAL NOT NULL,
    latency_p50_us      REAL NOT NULL,
    latency_p99_us      REAL NOT NULL,
    processing_delay_ms INTEGER NOT NULL,
    backlog             INTEGER NOT NULL,
    payments_in_memory  INTEGER NOT NULL,
    engine_clock_ms     INTEGER NOT NULL
);
"""

# When the same event is reported more than once, keep the most final status.
_STATUS_RANK = {"held": 1, "stale": 2, "applied": 3}


def connect(path: str | Path, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True, check_same_thread=False)
    else:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(path, check_same_thread=False)
        conn.execute("PRAGMA journal_mode=WAL")  # readers don't block the writer
        conn.executescript(SCHEMA)
    conn.row_factory = sqlite3.Row
    return conn


def apply_record(conn: sqlite3.Connection, rec: dict) -> None:
    kind = rec.get("type")
    if kind == "state":
        conn.execute(
            """INSERT INTO payments VALUES (:payment_id, :stage, :amount_minor, :currency, :last_update_ms,
                                            :invalid, :finished, :events_applied)
               ON CONFLICT(payment_id) DO UPDATE SET stage=excluded.stage, amount_minor=excluded.amount_minor,
                 currency=excluded.currency, last_update_ms=excluded.last_update_ms, invalid=excluded.invalid,
                 finished=excluded.finished, events_applied=excluded.events_applied""",
            rec,
        )
    elif kind == "event":
        if rec["status"] == "duplicate":
            # A flag, not a counter, so replaying the same record twice still gives the same answer.
            conn.execute("UPDATE events SET duplicated = 1 WHERE event_id = ?", (rec["event_id"],))
            return
        row = conn.execute("SELECT status FROM events WHERE event_id = ?", (rec["event_id"],)).fetchone()
        if row and _STATUS_RANK.get(row["status"], 0) >= _STATUS_RANK.get(rec["status"], 0):
            return
        conn.execute(
            """INSERT INTO events (event_id, payment_id, stage, timestamp_ms, amount_minor, currency, status, seen_at_ms)
               VALUES (:event_id, :payment_id, :stage, :timestamp_ms, :amount_minor, :currency, :status, :seen_at_ms)
               ON CONFLICT(event_id) DO UPDATE SET status=excluded.status, seen_at_ms=excluded.seen_at_ms""",
            rec,
        )
    elif kind == "alert":
        conn.execute(
            """INSERT INTO alerts VALUES (:alert_id, :alert_type, :severity, :payment_id, :stage, :raised_at_ms,
                                          :detail, :history_json, 'open', NULL)
               ON CONFLICT(alert_id) DO UPDATE SET status='open', cleared_at_ms=NULL, detail=excluded.detail,
                 history_json=excluded.history_json""",
            {**rec, "history_json": json.dumps(rec.get("history", []))},
        )
    elif kind == "alert_cleared":
        conn.execute(
            "UPDATE alerts SET status='cleared', cleared_at_ms=? WHERE alert_id=?",
            (rec["cleared_at_ms"], rec["alert_id"]),
        )
    elif kind == "metrics":
        conn.execute(
            """INSERT OR REPLACE INTO metrics VALUES (:wall_ms, :events_total, :events_per_sec, :latency_p50_us,
                 :latency_p99_us, :processing_delay_ms, :backlog, :payments_in_memory, :engine_clock_ms)""",
            rec,
        )
    # Unknown record types are ignored, so newer engines don't break older services.


def apply_records(conn: sqlite3.Connection, records: Iterable[dict]) -> int:
    n = 0
    with conn:  # one transaction
        for rec in records:
            apply_record(conn, rec)
            n += 1
    return n


# ---- Queries used by the API and the dashboard ---------------------------------------------


def _alert(row: sqlite3.Row) -> dict:
    d = dict(row)
    d["history"] = json.loads(d.pop("history_json"))
    return d


def list_alerts(
    conn: sqlite3.Connection,
    status: str | None = "open",
    alert_type: str | None = None,
    severity: str | None = None,
    limit: int = 100,
    offset: int = 0,
) -> list[dict]:
    where, args = [], []
    if status:
        where.append("status = ?")
        args.append(status)
    if alert_type:
        where.append("alert_type = ?")
        args.append(alert_type)
    if severity:
        where.append("severity = ?")
        args.append(severity)
    sql = "SELECT * FROM alerts"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY raised_at_ms DESC, alert_id LIMIT ? OFFSET ?"
    return [_alert(r) for r in conn.execute(sql, (*args, limit, offset))]


def get_payment(conn: sqlite3.Connection, payment_id: str) -> dict | None:
    row = conn.execute("SELECT * FROM payments WHERE payment_id = ?", (payment_id,)).fetchone()
    events = [
        {**dict(r), "duplicated": bool(r["duplicated"])}
        for r in conn.execute(
            "SELECT * FROM events WHERE payment_id = ? ORDER BY timestamp_ms, event_id", (payment_id,)
        )
    ]
    if row is None and not events:
        return None
    alerts = [
        _alert(r) for r in conn.execute("SELECT * FROM alerts WHERE payment_id = ? ORDER BY raised_at_ms", (payment_id,))
    ]
    state = dict(row) if row else None
    if state:
        state["invalid"] = bool(state["invalid"])
        state["finished"] = bool(state["finished"])
    return {"payment_id": payment_id, "state": state, "events": events, "alerts": alerts}


def stage_counts(conn: sqlite3.Connection) -> dict[str, int]:
    return {r["stage"]: r["n"] for r in conn.execute("SELECT stage, COUNT(*) AS n FROM payments GROUP BY stage")}


def alert_counts(conn: sqlite3.Connection, status: str = "open") -> dict[str, int]:
    return {
        r["alert_type"]: r["n"]
        for r in conn.execute("SELECT alert_type, COUNT(*) AS n FROM alerts WHERE status=? GROUP BY alert_type", (status,))
    }


def recent_metrics(conn: sqlite3.Connection, limit: int = 300) -> list[dict]:
    rows = conn.execute("SELECT * FROM metrics ORDER BY wall_ms DESC LIMIT ?", (limit,)).fetchall()
    return [dict(r) for r in reversed(rows)]
