from collections import Counter, defaultdict

import pytest

from sim.events import PATH
from sim.simulator import FAULTS, SimConfig, fault_counts, generate


@pytest.fixture(scope="module")
def run():
    events, labels = generate(SimConfig(payments=3000, seed=1))
    by_payment = defaultdict(list)
    for e in events:
        by_payment[e.payment_id].append(e)
    return events, {l.payment_id: l for l in labels}, by_payment


def test_same_seed_same_output():
    a = generate(SimConfig(payments=200, seed=5))
    b = generate(SimConfig(payments=200, seed=5))
    c = generate(SimConfig(payments=200, seed=6))
    assert a == b
    assert a != c


def test_every_fault_is_injected(run):
    _, labels, _ = run
    counts = Counter(l.fault for l in labels.values())
    for fault in FAULTS:
        assert counts[fault] > 40, fault
    assert counts["none"] > 2000


def test_healthy_payments_walk_every_stage_in_order(run):
    _, labels, by_payment = run
    for pid, label in labels.items():
        if label.fault != "none":
            continue
        evs = by_payment[pid]
        assert [e.stage for e in evs] == list(PATH)
        times = [e.timestamp_ms for e in evs]
        assert times == sorted(times)
        assert len({e.amount_minor for e in evs}) == 1


def test_normal_slow_payments_stay_under_the_limits(run):
    _, labels, by_payment = run
    slow = [pid for pid, l in labels.items() if l.note == "slow"]
    assert slow
    for pid in slow:
        t = [e.timestamp_ms for e in by_payment[pid]]
        assert max(b - a for a, b in zip(t, t[1:])) <= 3500


def test_each_fault_looks_right(run):
    _, labels, by_payment = run
    for pid, label in labels.items():
        evs = by_payment[pid]
        stages = [e.stage for e in evs]
        if label.fault == "stuck":
            assert len(evs) < 5 and "Settled" not in stages and "Failed" not in stages
        elif label.fault == "duplicate":
            assert len(evs) == 6 and len({e.event_id for e in evs}) == 5
        elif label.fault == "out_of_order":
            assert sorted(stages, key=PATH.index) == list(PATH)
            assert stages != list(PATH)  # delivered in the wrong order
        elif label.fault == "skipped_stage":
            assert len(evs) == 4 and label.fault_stage not in stages
        elif label.fault == "failed":
            assert stages[-1] == "Failed"
        elif label.fault == "amount_change":
            assert len({e.amount_minor for e in evs}) == 2


def test_event_ids_are_unique_apart_from_duplicates(run):
    events, labels, _ = run
    dupes = sum(1 for l in labels.values() if l.fault == "duplicate")
    assert len(events) - len({e.event_id for e in events}) == dupes


def test_events_mostly_arrive_in_time_order(run):
    events, _, _ = run
    late = sum(1 for a, b in zip(events, events[1:]) if b.timestamp_ms < a.timestamp_ms)
    assert late < len(events) * 0.1


def test_slowdown_makes_a_stage_slower_over_time():
    events, labels = generate(SimConfig(payments=2000, seed=3, fault_share=0, slowdown_stage="Screened"))
    by_payment = defaultdict(dict)
    for e in events:
        by_payment[e.payment_id][e.stage] = e.timestamp_ms
    gaps = [p["Sent"] - p["Screened"] for p in by_payment.values()]
    assert sum(gaps[-200:]) / 200 > 3 * sum(gaps[:200]) / 200
    assert fault_counts(labels)["none"] == 2000
