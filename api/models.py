"""Response shapes for the API (see docs/api.md)."""

from __future__ import annotations

from pydantic import BaseModel


class HistoryEntry(BaseModel):
    event_id: str
    stage: str
    timestamp_ms: int
    amount_minor: int


class AlertOut(BaseModel):
    alert_id: str
    alert_type: str
    severity: str
    payment_id: str | None
    stage: str | None
    raised_at_ms: int
    detail: str
    status: str
    cleared_at_ms: int | None
    history: list[HistoryEntry]


class AlertList(BaseModel):
    alerts: list[AlertOut]
    count: int
    limit: int
    offset: int


class PaymentStateOut(BaseModel):
    stage: str
    amount_minor: int
    currency: str
    last_update_ms: int
    invalid: bool
    finished: bool
    events_applied: int


class EventOut(BaseModel):
    event_id: str
    stage: str
    timestamp_ms: int
    amount_minor: int
    currency: str
    status: str
    seen_at_ms: int
    duplicated: bool


class PaymentOut(BaseModel):
    payment_id: str
    state: PaymentStateOut | None
    events: list[EventOut]
    alerts: list[AlertOut]


class Health(BaseModel):
    status: str
    payments: int
    open_alerts: int
    last_metrics_ms: int | None


class Stats(BaseModel):
    payments_by_stage: dict[str, int]
    open_alerts_by_type: dict[str, int]
    latest_metrics: dict | None


class ErrorBody(BaseModel):
    code: str
    message: str


class ErrorResponse(BaseModel):
    error: ErrorBody
