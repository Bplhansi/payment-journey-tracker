import pytest

from sim.inject import build
from sim.simulator import FAULTS


@pytest.mark.parametrize("fault", FAULTS)
def test_each_demo_fault(fault):
    plan = build(fault, 1_793_700_000_000)
    events = [e for _, e in plan]
    stages = [e.stage for e in events]
    assert len({e.payment_id for e in events}) == 1
    assert [o for o, _ in plan] == sorted(o for o, _ in plan)  # sent in order of the plan
    if fault == "stuck":
        assert stages == ["Received", "Validated", "Screened"]
    elif fault == "duplicate":
        assert len(events) == 6 and len({e.event_id for e in events}) == 5
    elif fault == "out_of_order":
        assert stages == ["Received", "Validated", "Sent", "Screened", "Settled"]
    elif fault == "skipped_stage":
        assert "Screened" not in stages
    elif fault == "failed":
        assert stages[-1] == "Failed"
    elif fault == "amount_change":
        assert len({e.amount_minor for e in events}) == 2
