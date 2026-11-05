import csv
import json

from sim.generate import main


def test_writes_events_and_labels(tmp_path, capsys):
    assert main(["--payments", "300", "--out", str(tmp_path)]) == 0
    lines = (tmp_path / "events.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) > 1000
    assert set(json.loads(lines[0])) == {"event_id", "payment_id", "stage", "timestamp_ms", "amount_minor", "currency"}
    with (tmp_path / "labels.csv").open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 300
    assert "wrote" in capsys.readouterr().out
