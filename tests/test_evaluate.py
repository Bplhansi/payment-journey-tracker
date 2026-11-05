from scripts.evaluate import score, slowdown_lead, to_markdown


def labels(**faults):
    return {pid: {"payment_id": pid, "fault": f, "fault_ms": "1000"} for pid, f in faults.items()}


def alert(pid, kind, at=1500, stage="Received"):
    return {"payment_id": pid, "alert_type": kind, "raised_at_ms": at, "stage": stage}


def test_counts_detected_missed_and_false_alarms():
    lab = labels(a="stuck", b="stuck", c="none", d="failed")
    alerts = [alert("a", "stuck", 5000), alert("c", "stuck"), alert("d", "failed", 1000)]
    by = {r.fault: r for r in score(lab, alerts)}
    assert (by["stuck"].injected, by["stuck"].detected, by["stuck"].false_alarms) == (2, 1, 1)
    assert by["stuck"].median_delay_ms == 4000
    assert by["failed"].recall == 1.0 and by["failed"].median_delay_ms == 0
    assert by["duplicate"].injected == 0 and by["duplicate"].median_delay_ms is None


def test_markdown_has_totals():
    text = to_markdown(score(labels(a="failed"), [alert("a", "failed")]))
    assert "| failed | 1 | 1 | 0 | 500 ms |" in text
    assert "Alert recall: 100.0%" in text


def test_slowdown_lead():
    lab = labels(a="none", b="stuck")
    alerts = [
        {"payment_id": None, "alert_type": "slowdown", "raised_at_ms": 100, "stage": "Screened"},
        alert("b", "stuck", 50, "Screened"),  # an injected stuck payment doesn't count
        alert("a", "stuck", 900, "Screened"),
    ]
    assert slowdown_lead(lab, alerts, "Screened") == (100, 900)
