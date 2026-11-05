import json

from api import db
from api.ingest import load_file, main, split_records


def test_split_records():
    assert split_records({"records": '{"a":1}\n{"b":2}'}) == ['{"a":1}', '{"b":2}']
    assert split_records({}) == []


def test_load_file_twice_is_safe(tmp_path):
    records = tmp_path / "records.jsonl"
    rows = [
        {"type": "state", "payment_id": "p", "stage": "Sent", "amount_minor": 5, "currency": "EUR",
         "last_update_ms": 3, "invalid": False, "finished": False, "events_applied": 4},
        {"type": "alert", "alert_id": "p:failed", "alert_type": "failed", "severity": "high", "payment_id": "p",
         "stage": "Sent", "raised_at_ms": 3, "detail": "x", "history": []},
    ]
    records.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    database = tmp_path / "t.db"
    assert load_file(database, records) == 2
    assert main(["--db", str(database), "--file", str(records)]) == 0
    c = db.connect(database)
    assert c.execute("SELECT COUNT(*) FROM payments").fetchone()[0] == 1
    assert c.execute("SELECT COUNT(*) FROM alerts").fetchone()[0] == 1
    c.close()
