"""The event format shared with the C++ engine (engine/include/pjt/event.hpp)."""

from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass

STAGES = ("Received", "Validated", "Screened", "Sent", "Settled", "Failed")
PATH = STAGES[:5]  # the normal journey, in order
_CURRENCY = re.compile(r"^[A-Z]{3}$")


@dataclass(frozen=True)
class Event:
    event_id: str
    payment_id: str
    stage: str
    timestamp_ms: int
    amount_minor: int  # whole minor units (pence, cents): never a float
    currency: str

    def __post_init__(self) -> None:
        if not self.event_id or not self.payment_id:
            raise ValueError("event_id and payment_id must not be empty")
        if self.stage not in STAGES:
            raise ValueError(f"unknown stage: {self.stage}")
        for name in ("timestamp_ms", "amount_minor"):
            value = getattr(self, name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"{name} must be a whole number >= 0")
        if not _CURRENCY.match(self.currency):
            raise ValueError("currency must be three capital letters")

    def to_json(self) -> str:
        """One compact JSON line, the exact format the engine reads."""
        return json.dumps(asdict(self), separators=(",", ":"))

    @classmethod
    def from_json(cls, line: str) -> Event:
        data = json.loads(line)
        return cls(**{k: data[k] for k in ("event_id", "payment_id", "stage", "timestamp_ms", "amount_minor", "currency")})
