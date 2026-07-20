from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple


@dataclass
class IntrusionOverlaySelection:
    status: str = "unavailable"
    region: Dict[str, float] = field(default_factory=dict)
    targets: List[Dict[str, Any]] = field(default_factory=list)
    recent_events: List[Dict[str, Any]] = field(default_factory=list)
    event_sequence: int = 0
    age_ms: float = 0.0
    reason: str = "event metadata unavailable"


def event_metadata_age_ms(events: Dict[str, Any], now_ms: Optional[float] = None) -> float:
    update_time_ms = events.get("update_time_ms")
    if isinstance(update_time_ms, (int, float)) and not isinstance(update_time_ms, bool) and update_time_ms > 0:
        current_ms = time.time() * 1000.0 if now_ms is None else now_ms
        return max(0.0, current_ms - float(update_time_ms))
    return 0.0


def select_intrusion_overlay(
    status: Dict[str, Any],
    events: Dict[str, Any],
    event_errors: Iterable[str],
    stale_threshold_ms: int,
    now_ms: Optional[float] = None,
    fallback_profile: Optional[Dict[str, Any]] = None,
) -> IntrusionOverlaySelection:
    errors = list(event_errors)
    region = _region(events) or _profile_region(fallback_profile)
    targets = _objects(events.get("in_region_targets"))
    recent_events = _objects(events.get("recent_events"))
    sequence = _positive_int(events.get("event_sequence"))
    age_ms = event_metadata_age_ms(events, now_ms)

    if errors:
        return IntrusionOverlaySelection(
            status="unavailable",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason="; ".join(errors),
        )
    if status.get("intrusion_enabled") is not True:
        return IntrusionOverlaySelection(
            status="unavailable",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason="intrusion not enabled",
        )
    if status.get("intrusion_state") in {"unavailable", "error", "starting"}:
        return IntrusionOverlaySelection(
            status="unavailable",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason=str(status.get("intrusion_error") or status.get("intrusion_state")),
        )
    if events.get("enabled") is not True or events.get("state") != "running":
        return IntrusionOverlaySelection(
            status="unavailable",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason=str(events.get("message") or events.get("state") or "event evaluator unavailable"),
        )
    if (
        not isinstance(events.get("update_time_ms"), (int, float))
        or isinstance(events.get("update_time_ms"), bool)
        or events.get("update_time_ms") <= 0
    ):
        return IntrusionOverlaySelection(
            status="unavailable",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason="event metadata has no update timestamp",
        )
    if age_ms > stale_threshold_ms:
        return IntrusionOverlaySelection(
            status="stale",
            region=region,
            targets=targets,
            recent_events=recent_events,
            event_sequence=sequence,
            age_ms=age_ms,
            reason="event metadata stale",
        )
    return IntrusionOverlaySelection(
        status="enabled",
        region=region,
        targets=targets,
        recent_events=recent_events,
        event_sequence=sequence,
        age_ms=age_ms,
        reason="event evaluator running",
    )


def intrusion_target_label(target: Dict[str, Any]) -> str:
    class_name = target.get("class_name", target.get("class", "obj"))
    track_id = target.get("track_id", "?")
    if target.get("state") == "alarmed":
        return f"INTRUSION {class_name} #{track_id}"
    dwell_ms = _number(target.get("dwell_ms"))
    threshold_ms = _number(target.get("threshold_ms"))
    return f"{class_name} #{track_id} {dwell_ms / 1000.0:.1f} / {threshold_ms / 1000.0:.1f}s"


def intrusion_has_alarm(targets: Iterable[Dict[str, Any]]) -> bool:
    return any(target.get("state") == "alarmed" for target in targets)


def advance_event_cursor(
    events: Dict[str, Any], previous_runtime_session_id: str, previous_sequence: int
) -> Tuple[str, int]:
    runtime_session_id = events.get("runtime_session_id")
    if not isinstance(runtime_session_id, str) or not runtime_session_id:
        runtime_session_id = previous_runtime_session_id
    sequence = _positive_int(events.get("event_sequence"))
    if runtime_session_id != previous_runtime_session_id:
        return runtime_session_id, sequence
    return runtime_session_id, max(previous_sequence, sequence)


def _objects(value: Any) -> List[Dict[str, Any]]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, dict)]


def _region(events: Dict[str, Any]) -> Dict[str, float]:
    source = events.get("region")
    if not isinstance(source, dict):
        return {}
    values: Dict[str, float] = {}
    for key in ("x1", "y1", "x2", "y2"):
        value = source.get(key)
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return {}
        values[key] = float(value)
    return values


def _profile_region(profile: Optional[Dict[str, Any]]) -> Dict[str, float]:
    if not isinstance(profile, dict):
        return {}
    rule = profile.get("rule")
    region = rule.get("region") if isinstance(rule, dict) else None
    if not isinstance(region, dict):
        return {}
    return _region({"region": region})


def _positive_int(value: Any) -> int:
    if isinstance(value, bool):
        return 0
    if isinstance(value, int) and value >= 0:
        return value
    if isinstance(value, float) and value >= 0:
        return int(value)
    return 0


def _number(value: Any) -> float:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    return 0.0
