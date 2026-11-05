"""Writes simulated events and their labels to a folder.

    python -m sim.generate --payments 5000 --out data
    python -m sim.generate --payments 5000 --out data/slowdown --slowdown Screened
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, fields
from pathlib import Path

from sim.events import PATH
from sim.simulator import Label, SimConfig, fault_counts, generate


def write(out: Path, cfg: SimConfig) -> dict[str, int]:
    events, labels = generate(cfg)
    out.mkdir(parents=True, exist_ok=True)
    with (out / "events.jsonl").open("w", encoding="utf-8", newline="\n") as f:
        for e in events:
            f.write(e.to_json() + "\n")
    with (out / "labels.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[x.name for x in fields(Label)])
        writer.writeheader()
        for label in labels:
            writer.writerow(asdict(label))
    counts = fault_counts(labels)
    counts["events"] = len(events)
    return counts


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--payments", type=int, default=5000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", type=Path, default=Path("data"))
    p.add_argument("--fault-share", type=float, default=0.03, help="share of payments per fault type")
    p.add_argument("--slowdown", choices=PATH[:4], help="gradually slow down this stage")
    args = p.parse_args(argv)
    if args.payments < 1:
        p.error("--payments must be at least 1")
    if not 0 <= args.fault_share <= 1 / 6:
        p.error("--fault-share must be between 0 and 0.166")

    cfg = SimConfig(payments=args.payments, seed=args.seed, fault_share=args.fault_share, slowdown_stage=args.slowdown)
    counts = write(args.out, cfg)
    print(f"wrote {counts.pop('events')} events for {args.payments} payments to {args.out}/events.jsonl")
    for fault, n in counts.items():
        print(f"  {fault:<14} {n:>6}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
