import json

import pytest

from sim.events import STAGES, Event


def test_matches_the_engine_format():
    e = Event("evt-000001", "pay-000001", "Received", 1793700000000, 125050, "USD")
    # The same line is read by the C++ test EventFormat.ReadsWhatPythonWrites.
    assert e.to_json() == (
        '{"event_id":"evt-000001","payment_id":"pay-000001","stage":"Received",'
        '"timestamp_ms":1793700000000,"amount_minor":125050,"currency":"USD"}'
    )


def test_round_trip():
    e = Event("evt-1", "pay-1", "Settled", 1, 0, "GBP")
    assert Event.from_json(e.to_json()) == e


def test_six_stages_in_order():
    assert STAGES == ("Received", "Validated", "Screened", "Sent", "Settled", "Failed")


@pytest.mark.parametrize(
    "changes",
    [
        {"stage": "Lost"},
        {"amount_minor": 12.5},  # money is never a float
        {"amount_minor": -1},
        {"timestamp_ms": "1"},
        {"currency": "gbp"},
        {"currency": "POUND"},
        {"event_id": ""},
        {"amount_minor": True},
    ],
)
def test_rejects_bad_values(changes):
    fields = {"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": 1, "currency": "GBP"}
    with pytest.raises(ValueError):
        Event(**{**fields, **changes})


def test_from_json_ignores_extra_fields():
    line = json.dumps({"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1,
                       "amount_minor": 1, "currency": "GBP", "extra": 1})
    assert Event.from_json(line).stage == "Sent"
