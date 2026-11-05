"""Sends simulated events into the Redis stream the engine reads (pjt:events).

    python -m sim.publish data/events.jsonl                 (real time: as the payments happen)
    python -m sim.publish data/events.jsonl --speed 10      (ten times faster)
    python -m sim.publish data/events.jsonl --speed 0       (as fast as possible: load test)

By default timestamps are shifted so the first event is "now"; the gaps between events
stay the same.
"""

from __future__ import annotations

import argparse
import os
import time
from dataclasses import replace
from pathlib import Path

from sim.events import Event

IN_STREAM = "pjt:events"


def read_events(path: Path) -> list[Event]:
    with path.open(encoding="utf-8") as f:
        return [Event.from_json(line) for line in f if line.strip()]


def publish(
    events: list[Event],
    redis_url: str,
    speed: float = 1.0,
    rebase: bool = True,
    stream: str = IN_STREAM,
    batch: int = 500,
) -> float:
    """Returns the seconds it took."""
    import redis

    r = redis.Redis.from_url(redis_url)
    if not events:
        return 0.0
    first_ts = min(e.timestamp_ms for e in events[:100])
    shift = int(time.time() * 1000) - first_ts if rebase else 0
    started = time.perf_counter()
    newest = first_ts
    pipe = r.pipeline(transaction=False)
    pending = 0
    for e in events:
        newest = max(newest, e.timestamp_ms)
        if speed > 0:  # wait until this moment of the simulation comes round
            due = (newest - first_ts) / 1000.0 / speed
            wait = due - (time.perf_counter() - started)
            if wait > 0:
                if pending:
                    pipe.execute()
                    pending = 0
                time.sleep(wait)
        out = replace(e, timestamp_ms=e.timestamp_ms + shift) if shift else e
        pipe.xadd(stream, {"data": out.to_json()})
        pending += 1
        if pending >= batch:
            pipe.execute()
            pending = 0
    if pending:
        pipe.execute()
    return time.perf_counter() - started


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("events", type=Path, nargs="?", default=Path("data/events.jsonl"))
    p.add_argument("--redis", default=os.environ.get("REDIS_URL", "redis://localhost:6379"))
    p.add_argument("--speed", type=float, default=1.0, help="1 = real time, 0 = as fast as possible")
    p.add_argument("--no-rebase", action="store_true", help="keep the original timestamps")
    args = p.parse_args(argv)
    if args.speed < 0:
        p.error("--speed must be 0 or more")
    events = read_events(args.events)
    secs = publish(events, args.redis, speed=args.speed, rebase=not args.no_rebase)
    print(f"published {len(events)} events in {secs:.1f} s ({len(events) / max(secs, 1e-9):,.0f} events/s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
