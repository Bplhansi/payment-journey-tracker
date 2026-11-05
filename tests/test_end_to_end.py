"""Runs the real C++ engine (pjt_replay) on simulated data and scores it. Skipped when the
engine hasn't been built yet."""

import subprocess
from pathlib import Path

import pytest

from scripts.engine_harness import ROOT, find_binary
from scripts.evaluate import read_alerts, read_labels, score
from sim.generate import write
from sim.simulator import SimConfig

try:
    REPLAY = find_binary("pjt_replay")
except FileNotFoundError:
    REPLAY = None

pytestmark = pytest.mark.skipif(REPLAY is None, reason="engine not built (pjt_replay missing)")


def replay(events: Path, out: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [REPLAY, str(events), "--config", str(ROOT / "config/engine.ini"), "--out", str(out), *extra],
        capture_output=True, text=True, check=False,
    )


def test_every_fault_caught_with_no_false_alarms(tmp_path):
    write(tmp_path, SimConfig(payments=3000, seed=11))
    assert replay(tmp_path / "events.jsonl", tmp_path / "records.jsonl").returncode == 0
    for r in score(read_labels(tmp_path / "labels.csv"), read_alerts(tmp_path / "records.jsonl")):
        assert r.detected == r.injected, r
        assert r.false_alarms == 0, r


def test_crash_and_resume_matches_clean_run(tmp_path):
    write(tmp_path, SimConfig(payments=2000, seed=12))
    events = tmp_path / "events.jsonl"
    assert replay(events, tmp_path / "clean.jsonl").returncode == 0
    snap = str(tmp_path / "snap")
    crashed = replay(events, tmp_path / "crash.jsonl", "--snapshot", snap, "--stop-after", "7777")
    assert crashed.returncode == 3  # stopped on purpose, like a crash
    assert replay(events, tmp_path / "crash.jsonl", "--snapshot", snap, "--resume").returncode == 0

    def finals(path):
        import json
        states, alerts = {}, set()
        for line in path.read_text(encoding="utf-8").splitlines():
            rec = json.loads(line)
            if rec["type"] == "state":
                states[rec["payment_id"]] = (rec["stage"], rec["amount_minor"], rec["invalid"], rec["finished"])
            elif rec["type"] == "alert":
                alerts.add(rec["alert_id"])
        return states, alerts

    assert finals(tmp_path / "crash.jsonl") == finals(tmp_path / "clean.jsonl")


def test_bad_input_is_reported(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"event_id": "e"}\n', encoding="utf-8")
    result = replay(bad, tmp_path / "out.jsonl")
    assert result.returncode == 1
    assert "line 1" in result.stderr
