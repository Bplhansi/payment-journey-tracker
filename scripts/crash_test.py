"""Kills the live engine halfway through a run, restarts it, and checks the final result
matches a run that never crashed.

    python -m scripts.crash_test --events data/events.jsonl

Uses its own Redis streams (pjt:crash:*), so it doesn't disturb a running demo.
"""

from __future__ import annotations

import argparse
import tempfile
import threading
import time
from pathlib import Path

import redis

from scripts.engine_harness import ROOT, Engine, caught_up, env_redis, group_info, outcome, read_records, replay_outcome, wait_for
from sim.publish import publish, read_events

IN, OUT = "pjt:crash:events", "pjt:crash:out"


def run(events_path: Path, redis_url: str, config: Path, kill_at: float, rate: float) -> dict:
    r = redis.Redis.from_url(redis_url)
    r.delete(IN, OUT)
    events = read_events(events_path)
    total = len(events)

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        clean_finals, clean_alerts = replay_outcome(events_path, config, work)
        engine = Engine(redis_url, IN, OUT, work / "engine.snapshot", config)

        # Publish at a steady pace in the background, like real traffic. Timestamps are kept as
        # they are, so the run is comparable with the replay. --speed is in "simulation seconds".
        publisher = threading.Thread(
            target=publish, args=(events, redis_url), kwargs={"speed": rate, "rebase": False, "stream": IN}, daemon=True
        )
        engine.start()
        publisher.start()
        target = int(total * kill_at)
        if not wait_for(lambda: group_info(r, IN).get("entries-read", 0) >= target, timeout_s=300):
            raise RuntimeError("engine never got going")
        engine.kill()
        crashed_at = group_info(r, IN).get("entries-read", 0)
        unacked = group_info(r, IN).get("pending", 0)
        print(f"killed the engine after it had read {crashed_at:,} of {total:,} events "
              f"({unacked:,} read but not yet covered by a snapshot)")
        time.sleep(1.0)  # traffic keeps arriving while it's down

        restarted = time.perf_counter()
        engine.start()
        # Recovery time: until the engine has worked through everything that piled up.
        if not wait_for(lambda: group_info(r, IN).get("entries-read", 0) > crashed_at
                        and group_info(r, IN).get("lag", 1) == 0, timeout_s=600, every_s=0.01):
            raise RuntimeError("engine did not catch up after the restart")
        recovery_s = time.perf_counter() - restarted
        if not wait_for(lambda: caught_up(r, IN, total) and not publisher.is_alive(), timeout_s=600):
            raise RuntimeError("engine did not finish after the restart")
        # Let the engine's clock run on past the last event, so any stuck payments are flagged
        # (the replay does the same at the end of the file).
        time.sleep(12)
        engine.stop()

        finals, alerts = outcome(read_records(r, OUT))
    r.delete(IN, OUT)

    matching = sum(1 for pid, state in clean_finals.items() if finals.get(pid) == state)
    return {
        "events": total,
        "killed_after": crashed_at,
        "recovery_s": recovery_s,
        "replayed": unacked,
        "payments": len(clean_finals),
        "matching": matching,
        "match_pct": 100.0 * matching / len(clean_finals),
        "alerts_clean": len(clean_alerts),
        "alerts_crash": len(alerts),
        "alerts_identical": alerts == clean_alerts,
        "missing_alerts": sorted(clean_alerts - alerts)[:5],
        "extra_alerts": sorted(alerts - clean_alerts)[:5],
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--events", type=Path, default=Path("data/events.jsonl"))
    p.add_argument("--redis", default=env_redis())
    p.add_argument("--config", type=Path, default=ROOT / "config/engine.ini")
    p.add_argument("--kill-at", type=float, default=0.4, help="share of events read before the crash")
    p.add_argument("--speed", type=float, default=10.0, help="publishing speed (x real time)")
    args = p.parse_args(argv)
    res = run(args.events, args.redis, args.config, args.kill_at, args.speed)
    print(f"recovery: caught up {res['recovery_s']:.2f} s after the restart, re-reading {res['replayed']:,} "
          "unacknowledged events plus everything that arrived while it was down")
    print(f"final states matching the clean run: {res['matching']:,} of {res['payments']:,} ({res['match_pct']:.2f}%)")
    print(f"alerts: {res['alerts_crash']:,} after crash vs {res['alerts_clean']:,} clean, identical: {res['alerts_identical']}")
    if not res["alerts_identical"]:
        print(f"  missing: {res['missing_alerts']}  extra: {res['extra_alerts']}")
    ok = res["matching"] == res["payments"] and res["alerts_identical"]
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
