"""Generates simulated payments, some with injected faults, plus a labels file.

Each payment walks Received -> Validated -> Screened -> Sent -> Settled with short random
gaps. A share of payments get exactly one fault. Events are returned in *delivery* order,
which is not always the order they happened in (that is the point of some faults).
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field

from sim.events import PATH, Event

FAULTS = ("stuck", "duplicate", "out_of_order", "skipped_stage", "failed", "amount_change")

# The alert the engine should raise for each fault.
EXPECTED_ALERT = {
    "stuck": "stuck",
    "duplicate": "duplicate",
    "out_of_order": "out_of_order",
    "skipped_stage": "invalid_journey",
    "failed": "failed",
    "amount_change": "amount_mismatch",
}

CURRENCIES = (("GBP", 0.5), ("EUR", 0.3), ("USD", 0.2))


@dataclass
class SimConfig:
    payments: int = 5000
    seed: int = 42
    start_ms: int = 1_793_700_000_000
    arrival_gap_ms: int = 20  # a new payment every 20 ms on average = 50 per second
    stage_gap_ms: tuple[int, int] = (100, 1200)  # normal time between stages
    slow_share: float = 0.02  # healthy payments that are slow but within limits
    slow_gap_ms: tuple[int, int] = (2000, 3500)
    fault_share: float = 0.03  # share of payments for EACH fault (6 faults = 18%)
    late_by_ms: tuple[int, int] = (50, 800)  # out of order: how late the early event arrives
    duplicate_after_ms: tuple[int, int] = (10, 2000)
    # Optional gradual slowdown of one stage (Step 12)
    slowdown_stage: str | None = None
    slowdown_from: float = 0.5  # starts this far through the run
    slowdown_per_payment_ms: float = 2.0  # extra time added per payment after it starts


@dataclass
class Label:
    payment_id: str
    fault: str  # one of FAULTS, or "none"
    fault_stage: str = ""
    fault_ms: int = 0  # when the fault happened (or should have been noticed from)
    note: str = ""


@dataclass
class _Delivery:
    deliver_ms: int
    seq: int
    event: Event = field(compare=False)


def _amount(rng: random.Random) -> int:
    # From 1.00 to 50,000.00, more small payments than large ones.
    return int(10 ** rng.uniform(2, 6.7))


def _currency(rng: random.Random) -> str:
    return rng.choices([c for c, _ in CURRENCIES], weights=[w for _, w in CURRENCIES])[0]


def generate(cfg: SimConfig) -> tuple[list[Event], list[Label]]:
    """Returns (events in delivery order, one label per payment)."""
    rng = random.Random(cfg.seed)
    deliveries: list[_Delivery] = []
    labels: list[Label] = []
    seq = 0
    event_no = 0
    start = cfg.start_ms
    slowdown_start = int(cfg.payments * cfg.slowdown_from)

    def deliver(at: int, e: Event) -> None:
        nonlocal seq
        deliveries.append(_Delivery(at, seq, e))
        seq += 1

    for i in range(1, cfg.payments + 1):
        pid = f"pay-{i:06d}"
        start += rng.randint(1, 2 * cfg.arrival_gap_ms - 1)
        amount = _amount(rng)
        currency = _currency(rng)

        roll = rng.random()
        fault = "none"
        if roll < cfg.fault_share * len(FAULTS):
            fault = FAULTS[int(roll / cfg.fault_share)]
        label = Label(pid, fault)

        # Times of each stage on the normal path.
        times = [start]
        slow_at = rng.randrange(1, 5) if fault == "none" and rng.random() < cfg.slow_share else -1
        for k in range(1, len(PATH)):
            gap = rng.randint(*cfg.slow_gap_ms) if k == slow_at else rng.randint(*cfg.stage_gap_ms)
            if cfg.slowdown_stage and i > slowdown_start and PATH[k - 1] == cfg.slowdown_stage:
                gap += int(cfg.slowdown_per_payment_ms * (i - slowdown_start))
            times.append(times[-1] + gap)
        if slow_at > 0:
            label.note = "slow"

        stages = list(PATH)
        amounts = [amount] * len(PATH)
        if fault == "stuck":
            stop = rng.randrange(0, 4)  # stops after Received..Sent
            stages, times = stages[: stop + 1], times[: stop + 1]
            label.fault_stage, label.fault_ms = stages[-1], times[-1]
        elif fault == "skipped_stage":
            k = rng.randrange(1, 4)  # Validated, Screened or Sent goes missing
            label.fault_stage, label.fault_ms = stages[k], times[k]
            del stages[k], times[k], amounts[k]
        elif fault == "failed":
            k = rng.randrange(0, 4)
            stages = stages[: k + 1] + ["Failed"]
            times = times[: k + 1] + [times[k] + rng.randint(*cfg.stage_gap_ms)]
            label.fault_stage, label.fault_ms = "Failed", times[-1]
        elif fault == "amount_change":
            k = rng.randrange(1, 5)
            new_amount = max(1, amount + rng.choice([-1, 1]) * rng.randint(1, max(1, amount // 10)))
            amounts = amounts[:k] + [new_amount] * (len(PATH) - k)
            label.fault_stage, label.fault_ms = stages[k], times[k]
        amounts = amounts[: len(stages)]

        events = []
        for stage, ts, amt in zip(stages, times, amounts, strict=True):
            event_no += 1
            events.append(Event(f"evt-{event_no:07d}", pid, stage, ts, amt, currency))

        late_index = -1
        if fault == "out_of_order":
            late_index = rng.randrange(1, 4)  # Validated, Screened or Sent arrives after the next one
            label.fault_stage = events[late_index].stage
            label.fault_ms = events[late_index + 1].timestamp_ms

        for k, e in enumerate(events):
            at = e.timestamp_ms
            if k == late_index:
                at = events[k + 1].timestamp_ms + rng.randint(*cfg.late_by_ms)
            deliver(at, e)

        if fault == "duplicate":
            k = rng.randrange(0, len(events))
            again = events[k].timestamp_ms + rng.randint(*cfg.duplicate_after_ms)
            deliver(again, events[k])
            label.fault_stage, label.fault_ms = events[k].stage, again

        labels.append(label)

    deliveries.sort(key=lambda d: (d.deliver_ms, d.seq))
    return [d.event for d in deliveries], labels


def fault_counts(labels: list[Label]) -> dict[str, int]:
    counts = {f: 0 for f in ("none", *FAULTS)}
    for label in labels:
        counts[label.fault] += 1
    return counts
