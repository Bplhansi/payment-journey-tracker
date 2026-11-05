import pytest

from api import db


@pytest.fixture
def c(tmp_path):
    conn = db.connect(tmp_path / "t.db")
    yield conn
    conn.close()


def state(stage="Received", **kw):
    return {"type": "state", "payment_id": "p1", "stage": stage, "amount_minor": 100, "currency": "GBP",
            "last_update_ms": 1, "invalid": False, "finished": False, "events_applied": 1, **kw}


def event(eid="e1", status="applied", **kw):
    return {"type": "event", "event_id": eid, "payment_id": "p1", "stage": "Received", "timestamp_ms": 1,
            "amount_minor": 100, "currency": "GBP", "status": status, "seen_at_ms": 1, **kw}


def alert(aid="p1:stuck:Received", **kw):
    return {"type": "alert", "alert_id": aid, "alert_type": "stuck", "severity": "high", "payment_id": "p1",
            "stage": "Received", "raised_at_ms": 5, "detail": "stuck", "history": [], **kw}


def test_applying_everything_twice_changes_nothing(tmp_path):
    records = [event(), event("e2", "held"), event("e2"), event("e1", "duplicate"), state(), alert(),
               {"type": "alert_cleared", "alert_id": "p1:stuck:Received", "cleared_at_ms": 9}]
    once, twice = db.connect(tmp_path / "a.db"), db.connect(tmp_path / "b.db")
    db.apply_records(once, records)
    db.apply_records(twice, records + records)
    dump = lambda c: [list(map(tuple, c.execute(f"SELECT * FROM {t} ORDER BY 1"))) for t in ("payments", "events", "alerts")]
    assert dump(once) == dump(twice)
    once.close()
    twice.close()


def test_event_keeps_its_most_final_status(c):
    db.apply_records(c, [event(status="held"), event(status="applied"), event(status="held")])
    assert c.execute("SELECT status FROM events").fetchone()[0] == "applied"


def test_duplicate_is_a_flag(c):
    db.apply_records(c, [event(), event(status="duplicate"), event(status="duplicate")])
    p = db.get_payment(c, "p1")
    assert len(p["events"]) == 1 and p["events"][0]["duplicated"] is True


def test_alert_open_then_cleared(c):
    db.apply_records(c, [alert()])
    assert [a["alert_id"] for a in db.list_alerts(c)] == ["p1:stuck:Received"]
    db.apply_records(c, [{"type": "alert_cleared", "alert_id": "p1:stuck:Received", "cleared_at_ms": 9}])
    assert db.list_alerts(c) == []
    assert db.list_alerts(c, status="cleared")[0]["cleared_at_ms"] == 9


def test_state_is_latest_and_counts_by_stage(c):
    db.apply_records(c, [state(), state("Validated", events_applied=2)])
    assert db.stage_counts(c) == {"Validated": 1}
    assert db.get_payment(c, "p1")["state"]["events_applied"] == 2


def test_unknown_payment_and_record_type(c):
    db.apply_records(c, [{"type": "something_new"}])
    assert db.get_payment(c, "nope") is None


def test_metrics(c):
    m = {"type": "metrics", "wall_ms": 1, "events_total": 10, "events_per_sec": 5.0, "latency_p50_us": 1.0,
         "latency_p99_us": 2.0, "processing_delay_ms": 3, "backlog": 0, "payments_in_memory": 4, "engine_clock_ms": 7}
    db.apply_records(c, [m, {**m, "wall_ms": 2}])
    assert [x["wall_ms"] for x in db.recent_metrics(c)] == [1, 2]
