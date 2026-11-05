# Alert API

A small read-only HTTP API over the alerts and payment journeys the engine reports.
Base URL when running locally: `http://localhost:8000`. Interactive docs: `/docs`.

All responses are JSON. Times are Unix time in milliseconds. Money is in whole minor units
(`amount_minor`, e.g. pence) with a three-letter `currency`.

## Endpoints

### `GET /v1/health`

Is the service up, and can it read the database?

| Status | When |
| --- | --- |
| 200 | Everything is fine |
| 503 | The database can't be read |

```json
{"status": "ok", "payments": 5000, "open_alerts": 312, "last_metrics_ms": 1793700123000}
```

### `GET /v1/alerts`

Alerts, newest first.

| Query parameter | Default | Meaning |
| --- | --- | --- |
| `status` | `open` | `open`, `cleared` or `all` |
| `type` | (any) | `stuck`, `invalid_journey`, `failed`, `amount_mismatch`, `duplicate`, `out_of_order`, `slowdown` |
| `severity` | (any) | `high`, `warning` or `info` |
| `limit` | `100` | 1 to 1000 |
| `offset` | `0` | for paging |

| Status | When |
| --- | --- |
| 200 | OK (the list may be empty) |
| 422 | A parameter has a value that isn't allowed |

```json
{
  "alerts": [
    {
      "alert_id": "pay-000045:stuck:Screened",
      "alert_type": "stuck",
      "severity": "high",
      "payment_id": "pay-000045",
      "stage": "Screened",
      "raised_at_ms": 1793700006350,
      "detail": "no new event for 5000 ms in Screened",
      "status": "open",
      "cleared_at_ms": null,
      "history": [
        {"event_id": "evt-0000201", "stage": "Received", "timestamp_ms": 1793700000600, "amount_minor": 25000}
      ]
    }
  ],
  "count": 1,
  "limit": 100,
  "offset": 0
}
```

### `GET /v1/payments/{payment_id}`

One payment's current state, its full journey (every event, in time order, with what the
engine did with it) and its alerts.

| Status | When |
| --- | --- |
| 200 | Found |
| 404 | No payment with that ID |
| 422 | The ID isn't valid (letters, digits, `-`, `_`, `.`, `:`; 1 to 64 characters) |

```json
{
  "payment_id": "pay-000045",
  "state": {"stage": "Screened", "amount_minor": 25000, "currency": "GBP", "last_update_ms": 1793700001350,
            "invalid": false, "finished": false, "events_applied": 3},
  "events": [
    {"event_id": "evt-0000201", "stage": "Received", "timestamp_ms": 1793700000600, "amount_minor": 25000,
     "currency": "GBP", "status": "applied", "seen_at_ms": 1793700000600, "duplicated": false}
  ],
  "alerts": []
}
```

Event `status` is `applied`, `held` (arrived early and is still waiting), or `stale`
(arrived after the payment had moved past it). `duplicated` is true when the same event
arrived more than once (it was still counted only once).

### `GET /v1/stats`

Totals for dashboards: payments per stage, open alerts per type, and the latest engine
metrics (events per second, processing delay, queue backlog, latency).

## Errors

Every error has the same shape:

```json
{"error": {"code": "not_found", "message": "no payment with ID pay-999999"}}
```

| Code | Status |
| --- | --- |
| `invalid_request` | 422 |
| `not_found` | 404 |
| `unavailable` | 503 |
