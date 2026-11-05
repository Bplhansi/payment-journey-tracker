"""Sends ONE new payment with a chosen fault into the live system, for demos.

    python -m sim.inject stuck
    python -m sim.inject amount_change

It prints the payment ID; search for it on the dashboard to watch the alert appear.
"""

from __future__ import annotations

import argparse
import os
import time

from sim.events import PATH, Event
from sim.publish import IN_STREAM
from sim.simulator import FAULTS


def build(fault: str, now_ms: int, gap_ms: int = 300) -> list[tuple[int, Event]]:
    """(send offset in ms, event) pairs for one payment with `fault`."""
    tag = time.strftime("%H%M%S", time.localtime(now_ms / 1000)) + f"{now_ms % 1000:03d}"
    pid = f"demo-{tag}"
    stages = list(PATH)
    amounts = [25000] * 5
    if fault == "stuck":
        stages = stages[:3]  # stops at Screened
    elif fault == "skipped_stage":
        del stages[2], amounts[2]  # no Screened
    elif fault == "failed":
        stages = stages[:3] + ["Failed"]
    elif fault == "amount_change":
        amounts = [25000, 25000, 25000, 27500, 27500]
    events = [
        Event(f"{pid}-e{k}", pid, s, now_ms + k * gap_ms, a, "GBP")
        for k, (s, a) in enumerate(zip(stages, amounts, strict=False))
    ]
    plan = [(e.timestamp_ms - now_ms, e) for e in events]
    if fault == "duplicate":
        plan.append((plan[1][0] + 50, events[1]))
    elif fault == "out_of_order":
        plan[2] = (plan[3][0] + 100, events[2])  # Screened arrives after Sent
    return sorted(plan, key=lambda x: x[0])


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("fault", choices=FAULTS)
    p.add_argument("--redis", default=os.environ.get("REDIS_URL", "redis://localhost:6379"))
    args = p.parse_args(argv)

    import redis

    r = redis.Redis.from_url(args.redis)
    now_ms = int(time.time() * 1000)
    plan = build(args.fault, now_ms)
    print(f"injecting a '{args.fault}' payment: {plan[0][1].payment_id}")
    start = time.perf_counter()
    for offset, e in plan:
        time.sleep(max(0.0, offset / 1000 - (time.perf_counter() - start)))
        r.xadd(IN_STREAM, {"data": e.to_json()})
        print(f"  sent {e.stage:<9} {e.event_id}")
    print("done. Search for that ID on the dashboard (stuck alerts appear after the stage's time limit).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
