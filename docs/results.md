# Results

All numbers below were measured on 2026-11-05 on a 4-core Intel Xeon @ 2.10 GHz cloud
machine with 16 GB of memory (Linux, GCC 13, Release build with `-O3`), using the default
settings in `config/engine.ini`. Re-run them on your own machine with the commands under each
section; numbers will differ from machine to machine.

## Alert quality

Every injected fault is labelled by the simulator (`data/labels.csv`), so the alerts can be
scored exactly.

**50,000 payments, seed 7** (243,930 events; 3% of payments get each fault):

| Fault | Injected | Detected | False alarms | Median detection delay |
| --- | ---: | ---: | ---: | ---: |
| stuck | 1529 | 1529 | 0 | 5,000 ms |
| duplicate | 1492 | 1492 | 0 | 0 ms |
| out_of_order | 1517 | 1517 | 0 | 426 ms |
| skipped_stage | 1527 | 1527 | 0 | 2,165 ms |
| failed | 1518 | 1518 | 0 | 0 ms |
| amount_change | 1574 | 1574 | 0 | 0 ms |

Alert recall: 100.0% (9157 of 9157). Alert precision: 100.0%.

**5,000 payments, seed 42** (the default `data/` set, 24,381 events): 917 of 917 faults
detected, 0 false alarms.

About 2% of healthy payments are deliberately slow (2 to 3.5 s in one stage, under every time
limit). None of them raised a stuck alert.

What the delays mean (all in payment time):

- **Stuck:** from the payment's last event to the alert, which is the stage's time limit
  (4 s, 4 s, 5 s or 6 s). The median is 5 s.
- **Skipped stage:** from when the missing event should have happened, through the next
  event arriving, plus the 1.5 s the engine waits for the missing one.
- **Out of order:** from when the overtaking event happened to when the late one arrived
  and both were applied.
- **Duplicate, failed, amount change:** caught on the event itself.

Re-run:

```bash
python -m sim.generate --payments 50000 --seed 7 --out data/big
engine/build/pjt_replay data/big/events.jsonl --out data/big/records.jsonl --repeat 5
python -m scripts.evaluate --labels data/big/labels.csv --records data/big/records.jsonl
```

## Engine speed (file replay, no queue)

`pjt_replay` reads the events into memory, then times each event through the engine
(including writing its output records as JSON).

| Data | Throughput (median of 5 runs) | Latency p50 | Latency p99 |
| --- | ---: | ---: | ---: |
| 5,000 payments (24,381 events) | 116,550 events/s (83,751 to 123,392) | 6.8 µs | 28.4 µs |
| 50,000 payments (243,930 events) | 110,955 events/s (102,264 to 115,954) | 7.3 µs | 34.0 µs |

## Live through Redis (load test)

`python -m scripts.load_test` starts its own `pjt_stream`, sends events at a fixed rate for
8 seconds per step (Python publisher, Redis 7.0, all on the same machine), and reads the
metrics the engine reports every second.

| Target (events/s) | Actually sent | Handled (median events/s) | Latency p50 | Latency p99 (worst second) | Processing delay (median) | Max backlog | Steady? |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 10,000 | 9,718 | 10,000 | 9.3 µs | 58.1 µs | 1 ms | 849 | yes |
| 20,000 | 19,836 | 19,801 | 8.7 µs | 52.3 µs | 2 ms | 2,700 | yes |
| 30,000 | 29,780 | 30,000 | 9.0 µs | 53.9 µs | 2 ms | 450 | yes |
| 40,000 | 39,736 | 40,000 | 8.8 µs | 63.6 µs | 38 ms | 10,800 | yes |
| 50,000 | 49,680 | 42,769 | 8.6 µs | 48.2 µs | 349 ms | 45,499 | no |
| 60,000 | 59,689 | 41,317 | 8.6 µs | 49.2 µs | 1,583 ms | 125,999 | no |

**Highest steady rate: 40,000 events/s.** Above about 43,000 events/s the queue grows.
"Processing delay" is the time from an event entering Redis to the engine handling it.

### What made it faster

The first version saved a snapshot after every 5,000 events. The load test showed the engine
topping out at about 8,000 events/s with the queue growing, even though one event takes only
about 8 µs to process. Timing a 195,000-event backlog showed snapshots were the cost: with
many payments in memory each snapshot is several megabytes of JSON.

| Change | Highest steady rate |
| --- | ---: |
| Snapshot every 5,000 events, one output write per record | about 8,000 events/s |
| Batch output (one Redis write per 1,000 records) | about 8,000 events/s (no change: not the bottleneck) |
| Snapshot at most once a second (`snapshot.every_ms`) | **40,000 events/s** |

The trade-off: after a crash, up to about a second of events is re-read. Duplicate handling
makes that harmless, and recovery still took well under a second.

Re-run (needs Redis and `pjt_stream`, so Linux or Docker):

```bash
python -m scripts.load_test --rates 10000 20000 30000 40000 50000 60000 --seconds 8
```

## Crash recovery

`python -m scripts.crash_test` publishes the 5,000-payment set at 10x real time, kills the
engine with `SIGKILL` (no chance to save anything), keeps publishing for a second while it
is down, restarts it, and compares the result with `pjt_replay` on the same events.

| Killed after | Unacknowledged events re-read | Time to catch up | Final states matching | Alerts |
| --- | ---: | ---: | ---: | --- |
| 40% of events (9,769 read) | 2,530 | 0.14 s | 5,000 of 5,000 (100%) | 917 of 917, identical |
| 75% of events (18,348 read) | 1,157 | 0.12 s | 5,000 of 5,000 (100%) | 917 of 917, identical |

The same check also runs without Redis, in the unit tests (crash at four different points,
`engine/tests/test_snapshot.cpp`) and in `tests/test_end_to_end.py` (stop and resume
`pjt_replay` from its snapshot).

## Slowdown detection (Step 12)

`python -m sim.generate --slowdown Screened` makes the Screened stage 2 ms slower with every
payment in the second half of the run. The detector compares a fast-moving and a
slow-moving average of each stage's time.

- First warning: "Screened now takes about 1063 ms, normally 705 ms".
- That was **23.7 s before** the fixed 5 s limit first fired on a payment that wasn't
  injected as stuck.
- On normal data (no slowdown) it raised no warnings, including on the 50,000-payment set.

```bash
python -m sim.generate --payments 5000 --out data/slow --slowdown Screened
engine/build/pjt_replay data/slow/events.jsonl --out data/slow/records.jsonl
python -m scripts.evaluate --labels data/slow/labels.csv --records data/slow/records.jsonl --slowdown-stage Screened
```

## Memory safety

All 39 C++ tests pass under AddressSanitizer + UndefinedBehaviorSanitizer and under
ThreadSanitizer (`sanitizers` CI job), with no errors reported. `pjt_replay` also ran the
full 5,000-payment set, and a stop-and-resume, under AddressSanitizer with no errors.

## Tests

- C++: 39 GoogleTest tests (state machine, event format, config, tracker, ordering and
  duplicates, stuck detection, slowdown, snapshots).
- Python: 48 pytest tests, 89% line coverage of `sim/`, `api/` and `scripts/` (the
  Redis-only scripts are covered by the integration CI job instead).
