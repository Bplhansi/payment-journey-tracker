# Two-minute demo script

Before recording: `docker compose up --build`, wait until the dashboard at
http://localhost:8501 shows payments moving (about a minute after start).

1. **What it is (15 s).** "This follows simulated payments through five stages and raises an
   alert when one gets stuck, fails, skips a stage, arrives twice or changes amount. All the
   data is simulated." Point at the headline numbers and the payments-per-stage chart.
2. **Alerts (20 s).** Open the alerts table, filter to `stuck`, click a payment ID, paste it
   into *Find a payment* and show its timeline: the last event, then nothing.
3. **Inject a fault live (25 s).** In a second terminal:
   `docker compose exec api python -m sim.inject stuck`. Copy the printed ID into the search.
   About five seconds later the stuck alert appears.
4. **Crash and recovery (40 s).** `docker compose kill engine`. Show events piling up (queue
   backlog rises). `docker compose start engine`. Show the log line
   `loaded snapshot covering stream ID ...` and the backlog falling back to zero. "It
   reloaded its last snapshot, replayed what it hadn't confirmed, and ignored the repeats.
   The crash test shows the final states match a run that never crashed: 100%."
5. **Numbers (20 s).** Show `docs/results.md`: every injected fault caught, no false alarms,
   steady events per second, p99 latency.
