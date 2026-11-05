"""Helpers shared by the crash test and the load test: start/stop the live engine and
read what it wrote. Needs Redis and a built pjt_stream (Linux, WSL or Docker)."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def find_binary(name: str) -> str:
    """Looks in $PATH, then the usual CMake build folders."""
    found = shutil.which(name)
    if found:
        return found
    for candidate in (ROOT / "engine/build" / name, ROOT / "engine/build/Release" / f"{name}.exe"):
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError(f"{name} not found: build the engine first (see README)")


class Engine:
    """One pjt_stream process reading `in_stream` and writing `out_stream`."""

    def __init__(self, redis_url: str, in_stream: str, out_stream: str, snapshot: Path, config: Path) -> None:
        self.cmd = [
            find_binary("pjt_stream"),
            "--redis", redis_url.replace("redis://", "tcp://"),
            "--in", in_stream,
            "--out", out_stream,
            "--snapshot", str(snapshot),
            "--config", str(config),
        ]
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        self.proc = subprocess.Popen(self.cmd, stderr=subprocess.PIPE, text=True)

    def kill(self) -> None:
        """Like pulling the plug: no chance to save anything."""
        assert self.proc
        self.proc.kill()
        self.proc.wait()

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def group_info(r, stream: str, group: str = "engine") -> dict:
    """entries-read, lag and pending for the engine's consumer group (empty if none yet)."""
    try:
        for g in r.xinfo_groups(stream):
            if g["name"] in (group, group.encode()):
                return g
    except Exception:  # noqa: BLE001 - the stream may not exist yet
        pass
    return {}


def caught_up(r, stream: str, total: int) -> bool:
    g = group_info(r, stream)
    return bool(g) and g.get("entries-read", 0) >= total and g.get("lag", 1) == 0 and g.get("pending", 1) == 0


def read_records(r, stream: str) -> list[dict]:
    records, last = [], "-"
    while True:
        batch = r.xrange(stream, min=last, max="+", count=10000)
        if last != "-":
            batch = batch[1:]  # xrange includes the starting ID
        if not batch:
            return records
        for _, fields in batch:
            for line in fields[b"records"].decode().split("\n"):
                if line:
                    records.append(json.loads(line))
        last = batch[-1][0]


def outcome(records: list[dict]) -> tuple[dict[str, tuple], set[str]]:
    """Final state of every payment, and every alert ID (repeats merged)."""
    finals: dict[str, tuple] = {}
    alerts: set[str] = set()
    for rec in records:
        if rec["type"] == "state":
            finals[rec["payment_id"]] = (rec["stage"], rec["amount_minor"], rec["invalid"], rec["finished"])
        elif rec["type"] == "alert":
            alerts.add(rec["alert_id"])
    return finals, alerts


def replay_outcome(events: Path, config: Path, work: Path) -> tuple[dict[str, tuple], set[str]]:
    """The reference answer: the same events through pjt_replay, no crash."""
    out = work / "clean.records.jsonl"
    subprocess.run(
        [find_binary("pjt_replay"), str(events), "--config", str(config), "--out", str(out)],
        check=True,
        capture_output=True,
    )
    with out.open(encoding="utf-8") as f:
        return outcome([json.loads(line) for line in f])


def wait_for(check, timeout_s: float, every_s: float = 0.05) -> bool:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if check():
            return True
        time.sleep(every_s)
    return False


def env_redis() -> str:
    return os.environ.get("REDIS_URL", "redis://localhost:6379")
