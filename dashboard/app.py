"""Live dashboard for payment health (see docs/dashboard-plan.md).

    streamlit run dashboard/app.py          (reads data/tracker.db, or $PJT_DB)
"""

from __future__ import annotations

import os
import sys
from datetime import UTC, datetime
from pathlib import Path

import pandas as pd
import streamlit as st

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # so "api" imports when run by streamlit
from api import db  # noqa: E402

DB_PATH = os.environ.get("PJT_DB", "data/tracker.db")
REFRESH = "2s"

st.set_page_config(page_title="Payment Journey Tracker", layout="wide")


def when(ms: int | None) -> str:
    if ms is None:
        return ""
    return datetime.fromtimestamp(ms / 1000, tz=UTC).strftime("%H:%M:%S.%f")[:-3]


def open_db():
    if not Path(DB_PATH).exists():
        return None
    return db.connect(DB_PATH, readonly=True)


st.title("Payment Journey Tracker")
st.caption("All payments here are simulated. No real payment data is used. Times are UTC.")


@st.fragment(run_every=REFRESH)
def headline_and_alerts() -> None:
    conn = open_db()
    if conn is None:
        st.info(f"Waiting for data: {DB_PATH} doesn't exist yet. Start the engine and the ingest service.")
        return
    try:
        counts = db.alert_counts(conn)
        metrics = db.recent_metrics(conn, limit=300)
        latest = metrics[-1] if metrics else {}
        high = sum(n for t, n in counts.items() if t in ("stuck", "invalid_journey", "failed", "amount_mismatch"))

        c1, c2, c3, c4, c5 = st.columns(5)
        c1.metric("Open problems", f"{high:,}", help="Stuck, invalid journey, failed and amount mismatch alerts")
        c2.metric("Stage slowdowns", counts.get("slowdown", 0))
        c3.metric("Events / second", f"{latest.get('events_per_sec', 0):,.0f}")
        c4.metric("Processing delay", f"{latest.get('processing_delay_ms', 0):,} ms",
                  help="Median time from an event entering the queue to the engine handling it")
        c5.metric("Queue backlog", f"{latest.get('backlog', 0):,}", help="Events waiting in Redis")

        left, right = st.columns([3, 2])
        with left:
            st.subheader("Open alerts")
            types = ["stuck", "invalid_journey", "failed", "amount_mismatch", "slowdown", "duplicate", "out_of_order"]
            chosen = st.multiselect("Types", types, default=types[:5], label_visibility="collapsed")
            rows = [a for a in db.list_alerts(conn, status="open", limit=500) if a["alert_type"] in chosen][:200]
            if rows:
                table = pd.DataFrame(
                    [
                        {
                            "raised": when(a["raised_at_ms"]),
                            "type": a["alert_type"],
                            "payment": a["payment_id"] or "(whole stage)",
                            "stage": a["stage"],
                            "detail": a["detail"],
                        }
                        for a in rows
                    ]
                )
                st.dataframe(table, hide_index=True, width="stretch", height=360)
            else:
                st.write("No open alerts of these types.")
            st.caption(
                f"Notices (not problems): {counts.get('duplicate', 0):,} duplicates counted once, "
                f"{counts.get('out_of_order', 0):,} late events applied in order."
            )
        with right:
            st.subheader("Payments per stage")
            stages = db.stage_counts(conn)
            order = ["Received", "Validated", "Screened", "Sent", "Settled", "Failed"]
            labels = [f"{i}. {s}" for i, s in enumerate(order, 1)]  # numbered, so the chart keeps this order
            st.bar_chart(pd.DataFrame({"payments": [stages.get(s, 0) for s in order]}, index=labels), height=300)

        if metrics:
            st.subheader("Engine")
            df = pd.DataFrame(metrics)
            df["time"] = pd.to_datetime(df["wall_ms"], unit="ms")
            df = df.set_index("time")
            m1, m2, m3 = st.columns(3)
            m1.caption("Events per second")
            m1.line_chart(df["events_per_sec"], height=180)
            m2.caption("Processing delay (ms) and queue backlog (events)")
            m2.line_chart(df[["processing_delay_ms", "backlog"]], height=180)
            m3.caption("Latency per event (µs): p50 and p99")
            m3.line_chart(df[["latency_p50_us", "latency_p99_us"]], height=180)
    finally:
        conn.close()


@st.fragment(run_every=REFRESH)
def payment_search() -> None:
    st.subheader("Find a payment")
    pid = st.text_input("Payment ID", placeholder="pay-000045 or demo-...", key="pid").strip()
    if not pid:
        return
    conn = open_db()
    if conn is None:
        return
    try:
        found = db.get_payment(conn, pid)
    finally:
        conn.close()
    if found is None:
        st.warning(f"No payment with ID {pid} yet.")
        return
    s = found["state"]
    if s:
        flags = []
        if s["invalid"]:
            flags.append("a stage was skipped")
        if not s["finished"]:
            flags.append("still in progress")
        st.write(
            f"**{s['stage']}** · {s['amount_minor'] / 100:,.2f} {s['currency']} · "
            f"last update {when(s['last_update_ms'])}" + (f" · {', '.join(flags)}" if flags else "")
        )
    timeline = pd.DataFrame(
        [
            {
                "happened": when(e["timestamp_ms"]),
                "stage": e["stage"],
                "amount": f"{e['amount_minor'] / 100:,.2f} {e['currency']}",
                "engine": e["status"] + (" (arrived twice)" if e["duplicated"] else ""),
                "event": e["event_id"],
            }
            for e in found["events"]
        ]
    )
    st.dataframe(timeline, hide_index=True, width="stretch")
    for a in found["alerts"]:
        st.error(f"{a['alert_type']} ({a['status']}) at {when(a['raised_at_ms'])}: {a['detail']}")


headline_and_alerts()
payment_search()
