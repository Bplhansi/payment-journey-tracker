# Dashboard plan

Who uses it: an operations person watching payment health. Each panel answers one question.

| Question | Panel |
| --- | --- |
| Is anything wrong right now? | Headline numbers: open problems (high severity), stage slowdowns, events per second |
| Which payments need attention, newest first? | Open alerts table, filterable by type |
| Where are payments piling up? | Payments per stage (bar chart) |
| What exactly happened to this payment? | Payment search: current state, timeline of every event, its alerts |
| Is the engine keeping up? | Events per second, processing delay and queue backlog over time |
| Is the engine fast enough? | p50 and p99 processing latency per event |

Rules:

- Refresh every 2 seconds without reloading the page.
- High-severity alerts first; duplicate and out-of-order notices are counted but kept out of
  the main table unless asked for.
- Every number says what it measures and in what unit.
- A clear note that all payment data is simulated.
