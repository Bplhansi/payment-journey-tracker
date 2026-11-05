"""FastAPI service: look up alerts and any payment's full journey.

    uvicorn api.app:app --port 8000        (reads data/tracker.db, or $PJT_DB)
"""

import os
import sqlite3
from collections.abc import Iterator
from enum import Enum
from typing import Annotated

from fastapi import Depends, FastAPI, Path, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from api import db
from api.models import AlertList, ErrorResponse, Health, PaymentOut, Stats


class AlertStatus(str, Enum):
    open = "open"
    cleared = "cleared"
    all = "all"


class AlertType(str, Enum):
    stuck = "stuck"
    invalid_journey = "invalid_journey"
    failed = "failed"
    amount_mismatch = "amount_mismatch"
    duplicate = "duplicate"
    out_of_order = "out_of_order"
    slowdown = "slowdown"


class Severity(str, Enum):
    high = "high"
    warning = "warning"
    info = "info"


def _error(status: int, code: str, message: str) -> JSONResponse:
    return JSONResponse(status_code=status, content={"error": {"code": code, "message": message}})


def create_app(db_path: str | None = None) -> FastAPI:
    path = db_path or os.environ.get("PJT_DB", "data/tracker.db")
    app = FastAPI(
        title="Payment Journey Tracker API",
        version="1.0.0",
        description="Alerts and payment journeys from the tracking engine. All payment data is simulated.",
    )
    errors = {404: {"model": ErrorResponse}, 422: {"model": ErrorResponse}}

    def get_conn() -> Iterator[sqlite3.Connection]:
        # One connection per request, always closed. connect() creates the tables if the file is new.
        c = db.connect(path)
        try:
            yield c
        finally:
            c.close()

    Conn = Annotated[sqlite3.Connection, Depends(get_conn)]

    @app.exception_handler(RequestValidationError)
    async def invalid(_: Request, exc: RequestValidationError) -> JSONResponse:
        first = exc.errors()[0] if exc.errors() else {}
        where = ".".join(str(p) for p in first.get("loc", []) if p not in ("query", "path"))
        return _error(422, "invalid_request", f"{where}: {first.get('msg', 'invalid value')}")

    @app.exception_handler(sqlite3.Error)
    async def db_down(_: Request, exc: sqlite3.Error) -> JSONResponse:
        return _error(503, "unavailable", "the alert database can't be read right now")

    @app.get("/v1/health", response_model=Health, responses={503: {"model": ErrorResponse}})
    def health(c: Conn) -> Health:
        payments = c.execute("SELECT COUNT(*) FROM payments").fetchone()[0]
        open_alerts = c.execute("SELECT COUNT(*) FROM alerts WHERE status='open'").fetchone()[0]
        last = c.execute("SELECT MAX(wall_ms) FROM metrics").fetchone()[0]
        return Health(status="ok", payments=payments, open_alerts=open_alerts, last_metrics_ms=last)

    @app.get("/v1/alerts", response_model=AlertList, responses=errors)
    def alerts(
        c: Conn,
        status: AlertStatus = AlertStatus.open,
        type: AlertType | None = None,  # noqa: A002 - matches the public query name
        severity: Severity | None = None,
        limit: Annotated[int, Query(ge=1, le=1000)] = 100,
        offset: Annotated[int, Query(ge=0)] = 0,
    ) -> AlertList:
        rows = db.list_alerts(
            c,
            status=None if status is AlertStatus.all else status.value,
            alert_type=type.value if type else None,
            severity=severity.value if severity else None,
            limit=limit,
            offset=offset,
        )
        return AlertList(alerts=rows, count=len(rows), limit=limit, offset=offset)

    @app.get("/v1/payments/{payment_id}", response_model=PaymentOut, responses=errors)
    def payment(
        c: Conn,
        payment_id: Annotated[str, Path(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_.:\-]+$")],
    ) -> PaymentOut | JSONResponse:
        found = db.get_payment(c, payment_id)
        if found is None:
            return _error(404, "not_found", f"no payment with ID {payment_id}")
        return PaymentOut(**found)

    @app.get("/v1/stats", response_model=Stats)
    def stats(c: Conn) -> Stats:
        metrics = db.recent_metrics(c, limit=1)
        return Stats(
            payments_by_stage=db.stage_counts(c),
            open_alerts_by_type=db.alert_counts(c),
            latest_metrics=metrics[-1] if metrics else None,
        )

    return app


app = create_app()
