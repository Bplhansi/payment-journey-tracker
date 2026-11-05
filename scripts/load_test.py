"""Finds the highest event rate the live engine can keep up with.

    python -m scripts.load_test --rates 5000 10000 20000 40000

For each rate it sends events at that pace for a few seconds, then reports what the engine
measured: events per second handled, processing latency (p50/p99) and queue backlog.
A rate is "steady" when the backlog stays small (under a tenth of a second of traffic) at the
end of the step. Uses its own streams (pjt:load:*).
"""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import tempfile
import time
from pathlib import Path

import redis

from scripts.engine_harness import ROOT, Engine, env_redis, group_info, wait_for
from sim.simulator import SimConfig, generate

IN, OUT = "pjt:load:events", "pjt:load:out"


def send_at_rate(r, lines: list[str], rate: int, seconds: float) -> int:
    """Sends up to rate*seconds events, in small batches, at `rate` per second."""
    batch = max(1, rate // 200)  # 200 batches a second
    total = min(len(lines), int(rate * seconds))
    start = time.perf_counter()
    sent = 0
    pipe = r.pipeline(transaction=False)
    while sent < total:
        for line in lines[sent : sent + batch]:
            pipe.xadd(IN, {"data": line})
        pipe.execute()
        sent += min(batch, total - sent)
        wait = sent / rate - (time.perf_counter() - start)
        if wait > 0:
            time.sleep(wait)
    return sent


def metrics_since(r, since_ms: int) -> list[dict]:
    out = []
    for _, fields in r.xrange(OUT, min=f"{since_ms}-0", max="+"):
        text = fields[b"records"].decode()
        if '"type":"metrics"' not in text:
            continue
        for line in text.split("\n"):
            rec = json.loads(line)
            if rec["type"] == "metrics":
                out.append(rec)
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--redis", default=env_redis())
    p.add_argument("--config", type=Path, default=ROOT / "config/engine.ini")
    p.add_argument("--rates", type=int, nargs="+", default=[5000, 10000, 20000, 40000, 80000])
    p.add_argument("--seconds", type=float, default=8.0, help="how long to hold each rate")
    args = p.parse_args(argv)

    r = redis.Redis.from_url(args.redis)
    need = int(max(args.rates) * args.seconds * len(args.rates))
    payments = need // 5 + 1000
    print(f"generating {payments:,} payments...", flush=True)
    events, _ = generate(SimConfig(payments=payments, seed=7))
    lines = [e.to_json() for e in events]

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        r.delete(IN, OUT)
        engine = Engine(args.redis, IN, OUT, Path(tmp) / "engine.snapshot", args.config)
        engine.start()
        try:
            offset = 0
            for rate in args.rates:
                step_start = int(time.time() * 1000)
                t0 = time.perf_counter()
                sent = send_at_rate(r, lines[offset:], rate, args.seconds)
                actual = sent / (time.perf_counter() - t0)
                offset += sent
                lag_at_end = group_info(r, IN).get("lag", -1)
                wait_for(lambda: group_info(r, IN).get("lag", 1) == 0, timeout_s=120)
                time.sleep(1.2)  # one more metrics tick
                m = [x for x in metrics_since(r, step_start) if x["events_per_sec"] > 0]
                rows.append(
                    {
                        "rate": rate,
                        "actual": actual,
                        "handled": statistics.median(x["events_per_sec"] for x in m) if m else 0,
                        "p50_us": statistics.median(x["latency_p50_us"] for x in m) if m else 0,
                        "p99_us": max(x["latency_p99_us"] for x in m) if m else 0,
                        "delay_ms": statistics.median(x["processing_delay_ms"] for x in m) if m else 0,
                        "max_backlog": max((x["backlog"] for x in m), default=0),
                        "lag_at_end": lag_at_end,
                    }
                )
                row = rows[-1]
                print(f"{rate:>7,}/s asked, {actual:,.0f}/s sent -> handled {row['handled']:>8,.0f}/s, backlog at end {lag_at_end:,}", flush=True)
        finally:
            engine.stop()
            r.delete(IN, OUT)

    print()
    print(f"Machine: {platform.platform()}, {platform.processor() or platform.machine()}")
    print("| Target (events/s) | Actually sent | Handled (median events/s) | Latency p50 | Latency p99 (worst second) | Processing delay (median) | Max backlog | Steady? |")
    print("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
    best = 0
    for row in rows:
        # Steady: the sender really hit the target, and the engine didn't fall behind.
        steady = row["actual"] >= 0.95 * row["rate"] and 0 <= row["lag_at_end"] <= row["rate"] / 10
        if steady:
            best = max(best, row["rate"])
        print(
            f"| {row['rate']:,} | {row['actual']:,.0f} | {row['handled']:,.0f} | {row['p50_us']:.1f} µs | {row['p99_us']:.1f} µs | "
            f"{row['delay_ms']:,.0f} ms | {row['max_backlog']:,} | {'yes' if steady else 'no'} |"
        )
    print()
    print(f"Highest steady rate tested: {best:,} events/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
