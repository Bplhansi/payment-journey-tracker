import pytest
from fastapi.testclient import TestClient

from api import db
from api.app import create_app


@pytest.fixture
def client(tmp_path):
    path = tmp_path / "t.db"
    c = db.connect(path)
    db.apply_records(
        c,
        [
            {"type": "event", "event_id": "e1", "payment_id": "pay-1", "stage": "Received", "timestamp_ms": 10,
             "amount_minor": 2500, "currency": "GBP", "status": "applied", "seen_at_ms": 10},
            {"type": "state", "payment_id": "pay-1", "stage": "Received", "amount_minor": 2500, "currency": "GBP",
             "last_update_ms": 10, "invalid": False, "finished": False, "events_applied": 1},
            {"type": "alert", "alert_id": "pay-1:stuck:Received", "alert_type": "stuck", "severity": "high",
             "payment_id": "pay-1", "stage": "Received", "raised_at_ms": 4010, "detail": "no new event",
             "history": [{"event_id": "e1", "stage": "Received", "timestamp_ms": 10, "amount_minor": 2500}]},
            {"type": "alert", "alert_id": "pay-1:duplicate", "alert_type": "duplicate", "severity": "info",
             "payment_id": "pay-1", "stage": "Received", "raised_at_ms": 20, "detail": "dup", "history": []},
        ],
    )
    c.close()
    return TestClient(create_app(str(path)))


def test_health(client):
    body = client.get("/v1/health").json()
    assert body == {"status": "ok", "payments": 1, "open_alerts": 2, "last_metrics_ms": None}


def test_alerts_newest_first_and_filters(client):
    body = client.get("/v1/alerts").json()
    assert [a["alert_id"] for a in body["alerts"]] == ["pay-1:stuck:Received", "pay-1:duplicate"]
    assert body["alerts"][0]["history"][0]["event_id"] == "e1"
    assert client.get("/v1/alerts", params={"type": "stuck"}).json()["count"] == 1
    assert client.get("/v1/alerts", params={"severity": "info"}).json()["count"] == 1
    assert client.get("/v1/alerts", params={"status": "cleared"}).json()["count"] == 0
    assert client.get("/v1/alerts", params={"limit": 1, "offset": 1}).json()["alerts"][0]["alert_id"] == "pay-1:duplicate"


def test_payment_journey(client):
    body = client.get("/v1/payments/pay-1").json()
    assert body["state"]["stage"] == "Received"
    assert [e["event_id"] for e in body["events"]] == ["e1"]
    assert len(body["alerts"]) == 2


def test_errors_share_one_format(client):
    r = client.get("/v1/payments/pay-404")
    assert r.status_code == 404
    assert r.json() == {"error": {"code": "not_found", "message": "no payment with ID pay-404"}}
    for url in ("/v1/payments/bad%20id", "/v1/alerts?limit=0", "/v1/alerts?type=lost", "/v1/alerts?status=x"):
        r = client.get(url)
        assert r.status_code == 422, url
        assert r.json()["error"]["code"] == "invalid_request"


def test_stats(client):
    body = client.get("/v1/stats").json()
    assert body["payments_by_stage"] == {"Received": 1}
    assert body["open_alerts_by_type"] == {"stuck": 1, "duplicate": 1}


def test_empty_database_still_answers(tmp_path):
    client = TestClient(create_app(str(tmp_path / "new.db")))
    assert client.get("/v1/health").json()["payments"] == 0
