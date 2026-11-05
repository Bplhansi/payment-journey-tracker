"""Scores the engine's alerts against the simulator's labels.

    python -m scripts.evaluate --labels data/labels.csv --records data/records.jsonl

For each fault: how many were injected, how many the engine caught, how many alerts of
that type hit payments without the fault (false alarms), and the median time from the
fault to its alert.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from sim.simulator import EXPECTED_ALERT, FAULTS


@dataclass
class FaultScore:
    fault: str
    injected: int
    detected: int
    false_alarms: int
    median_delay_ms: float | None

    @property
    def recall(self) -> float:
        return self.detected / self.injected if self.injected else 1.0

    @property
    def precision(self) -> float:
        alerts = self.detected + self.false_alarms
        return self.detected / alerts if alerts else 1.0


def read_labels(path: Path) -> dict[str, dict[str, str]]:
    with path.open(encoding="utf-8") as f:
        return {row["payment_id"]: row for row in csv.DictReader(f)}


def read_alerts(path: Path) -> list[dict]:
    """Alert records from an engine output file. Repeats (after a crash) are merged by ID."""
    alerts: dict[str, dict] = {}
    with path.open(encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            if rec.get("type") == "alert" and rec["alert_id"] not in alerts:
                alerts[rec["alert_id"]] = rec
    return list(alerts.values())


def score(labels: dict[str, dict[str, str]], alerts: list[dict]) -> list[FaultScore]:
    # First alert time per (payment, alert type)
    first: dict[tuple[str, str], int] = {}
    by_type: dict[str, set[str]] = defaultdict(set)
    for a in alerts:
        pid = a.get("payment_id")
        if not pid:
            continue  # stage-wide alerts (slowdown) aren't about one payment
        key = (pid, a["alert_type"])
        first[key] = min(first.get(key, a["raised_at_ms"]), a["raised_at_ms"])
        by_type[a["alert_type"]].add(pid)

    results = []
    for fault in FAULTS:
        alert_type = EXPECTED_ALERT[fault]
        faulty = {pid for pid, row in labels.items() if row["fault"] == fault}
        flagged = by_type.get(alert_type, set())
        caught = faulty & flagged
        # The engine clock is the newest event time it has seen, which can trail the moment a
        # duplicate arrived by a few ms. Those are caught on arrival, so count them as 0.
        delays = [max(0, first[(pid, alert_type)] - int(labels[pid]["fault_ms"])) for pid in caught]
        results.append(
            FaultScore(
                fault=fault,
                injected=len(faulty),
                detected=len(caught),
                false_alarms=len(flagged - faulty),
                median_delay_ms=statistics.median(delays) if delays else None,
            )
        )
    return results


def to_markdown(results: list[FaultScore]) -> str:
    lines = [
        "| Fault | Injected | Detected | False alarms | Median detection delay |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for r in results:
        delay = "n/a" if r.median_delay_ms is None else f"{r.median_delay_ms:,.0f} ms"
        lines.append(f"| {r.fault} | {r.injected} | {r.detected} | {r.false_alarms} | {delay} |")
    injected = sum(r.injected for r in results)
    detected = sum(r.detected for r in results)
    false_alarms = sum(r.false_alarms for r in results)
    recall = detected / injected if injected else 1.0
    precision = detected / (detected + false_alarms) if detected + false_alarms else 1.0
    lines.append("")
    lines.append(f"Alert recall: {recall:.1%} ({detected} of {injected}). Alert precision: {precision:.1%}.")
    return "\n".join(lines)


def slowdown_lead(labels: dict[str, dict[str, str]], alerts: list[dict], stage: str) -> tuple[int | None, int | None]:
    """When the first slowdown warning for `stage` came, and when the first stuck alert in that
    stage hit a payment that wasn't injected as stuck (i.e. the fixed limit catching it)."""
    warned = [a["raised_at_ms"] for a in alerts if a["alert_type"] == "slowdown" and a.get("stage") == stage]
    limit_hit = [
        a["raised_at_ms"]
        for a in alerts
        if a["alert_type"] == "stuck" and a.get("stage") == stage and labels.get(a["payment_id"], {}).get("fault") != "stuck"
    ]
    return (min(warned) if warned else None, min(limit_hit) if limit_hit else None)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--labels", type=Path, default=Path("data/labels.csv"))
    p.add_argument("--records", type=Path, default=Path("data/records.jsonl"))
    p.add_argument("--slowdown-stage", help="also report how early a slowdown in this stage was flagged")
    args = p.parse_args(argv)
    labels, alerts = read_labels(args.labels), read_alerts(args.records)
    print(to_markdown(score(labels, alerts)))
    if args.slowdown_stage:
        warned, limit_hit = slowdown_lead(labels, alerts, args.slowdown_stage)
        print()
        if warned is None:
            print(f"No slowdown warning for {args.slowdown_stage}.")
        elif limit_hit is None:
            print(f"Slowdown warning for {args.slowdown_stage}; the fixed limit never fired on a healthy payment.")
        else:
            print(f"Slowdown warning for {args.slowdown_stage} came {(limit_hit - warned) / 1000:,.1f} s "
                  "before the fixed time limit first fired on a payment that wasn't injected as stuck.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
