"""Moves engine output into SQLite.

    python -m api.ingest --file data/records.jsonl          (after a pjt_replay run)
    python -m api.ingest --redis redis://localhost:6379     (live: follows the pjt:out stream)

In live mode it reads as a Redis consumer group and acknowledges each batch only after it
is committed to SQLite, so a crash here loses nothing either.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from api.db import apply_records, connect

OUT_STREAM = "pjt:out"
GROUP = "ingest"


def split_records(fields: dict) -> list[str]:
    """One output stream entry holds one or more JSON records, one per line."""
    return [line for line in fields.get("records", "").split("\n") if line]


def load_file(db_path: Path, records_path: Path) -> int:
    conn = connect(db_path)
    try:
        with records_path.open(encoding="utf-8") as f:
            return apply_records(conn, (json.loads(line) for line in f if line.strip()))
    finally:
        conn.close()


def follow(db_path: Path, redis_url: str, consumer: str = "ingest-1") -> None:
    import redis  # only needed in live mode

    r = redis.Redis.from_url(redis_url, decode_responses=True)
    conn = connect(db_path)
    try:
        r.xgroup_create(OUT_STREAM, GROUP, id="0", mkstream=True)
    except redis.ResponseError as e:
        if "BUSYGROUP" not in str(e):
            raise
    print(f"ingest: following {OUT_STREAM} into {db_path}", flush=True)
    start_id = "0"  # first, anything we read before a restart but never acknowledged
    while True:
        got = r.xreadgroup(GROUP, consumer, {OUT_STREAM: start_id}, count=1000, block=1000)
        entries = got[0][1] if got else []
        if start_id == "0" and not entries:
            start_id = ">"  # backlog done: now only new records
            continue
        if not entries:
            continue
        apply_records(conn, (json.loads(line) for _, fields in entries for line in split_records(fields)))
        r.xack(OUT_STREAM, GROUP, *[entry_id for entry_id, _ in entries])


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", type=Path, default=Path(os.environ.get("PJT_DB", "data/tracker.db")))
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--file", type=Path, help="an engine output file (JSON lines)")
    src.add_argument("--redis", help="Redis URL to follow live, e.g. redis://localhost:6379")
    args = p.parse_args(argv)
    if args.file:
        n = load_file(args.db, args.file)
        print(f"stored {n} records in {args.db}")
    else:
        follow(args.db, args.redis)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
