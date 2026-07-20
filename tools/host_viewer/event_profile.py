from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Union


EVENT_PROFILE_SCHEMA_VERSION = 1


class EventProfileError(ValueError):
    """Raised when a startup event profile is not safe to send to the board."""


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise EventProfileError(f"{field} must be a finite number")
    return float(value)


def _positive_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise EventProfileError(f"{field} must be a positive integer")
    return value


def resolve_event_profile(source: Mapping[str, Any], expected_camera_id: Optional[str] = None) -> Dict[str, Any]:
    allowed = {"event_profile_schema_version", "camera_id", "enabled", "rule"}
    unknown = sorted(set(source) - allowed)
    if unknown:
        raise EventProfileError(f"event profile contains unknown field: {unknown[0]}")
    missing = sorted(allowed - set(source))
    if missing:
        raise EventProfileError(f"event profile is missing field: {missing[0]}")
    if source["event_profile_schema_version"] != EVENT_PROFILE_SCHEMA_VERSION:
        raise EventProfileError(
            f"unsupported event_profile_schema_version {source['event_profile_schema_version']!r}"
        )
    camera_id = source["camera_id"]
    if not isinstance(camera_id, str) or not camera_id.strip():
        raise EventProfileError("event profile camera_id must be non-empty")
    camera_id = camera_id.strip()
    if expected_camera_id is not None and camera_id != expected_camera_id:
        raise EventProfileError(
            f"event profile camera_id {camera_id!r} does not match {expected_camera_id!r}"
        )
    if not isinstance(source["enabled"], bool):
        raise EventProfileError("event profile enabled must be boolean")

    rule_source = source["rule"]
    if not isinstance(rule_source, Mapping):
        raise EventProfileError("event profile rule must be an object")
    rule_allowed = {
        "rule_id",
        "type",
        "class_ids",
        "region",
        "dwell_ms",
        "boundary_hysteresis_px",
        "prediction_grace_ms",
    }
    unknown = sorted(set(rule_source) - rule_allowed)
    if unknown:
        raise EventProfileError(f"event profile rule contains unknown field: {unknown[0]}")
    missing = sorted(rule_allowed - set(rule_source))
    if missing:
        raise EventProfileError(f"event profile rule is missing field: {missing[0]}")

    rule_id = rule_source["rule_id"]
    if not isinstance(rule_id, str) or not rule_id.strip():
        raise EventProfileError("event profile rule_id must be non-empty")
    if rule_source["type"] != "region_intrusion":
        raise EventProfileError("event profile rule.type must be region_intrusion")

    class_ids = rule_source["class_ids"]
    if (
        not isinstance(class_ids, list)
        or not class_ids
        or any(isinstance(value, bool) or not isinstance(value, int) or value < 0 for value in class_ids)
        or len(set(class_ids)) != len(class_ids)
    ):
        raise EventProfileError("event profile rule.class_ids must be unique non-negative integers")

    region_source = rule_source["region"]
    if not isinstance(region_source, Mapping):
        raise EventProfileError("event profile rule.region must be an object")
    region_allowed = {"type", "x1", "y1", "x2", "y2"}
    unknown = sorted(set(region_source) - region_allowed)
    if unknown:
        raise EventProfileError(f"event profile region contains unknown field: {unknown[0]}")
    missing = sorted(region_allowed - set(region_source))
    if missing:
        raise EventProfileError(f"event profile region is missing field: {missing[0]}")
    if region_source["type"] != "rectangle":
        raise EventProfileError("event profile region.type must be rectangle")
    region = {key: _number(region_source[key], f"event profile region.{key}") for key in ("x1", "y1", "x2", "y2")}
    if not (
        0.0 <= region["x1"] < region["x2"] <= 1.0
        and 0.0 <= region["y1"] < region["y2"] <= 1.0
    ):
        raise EventProfileError("event profile region must be an ordered normalized rectangle")

    dwell_ms = _positive_integer(rule_source["dwell_ms"], "event profile rule.dwell_ms")
    hysteresis = _number(
        rule_source["boundary_hysteresis_px"], "event profile rule.boundary_hysteresis_px"
    )
    if hysteresis < 0:
        raise EventProfileError("event profile rule.boundary_hysteresis_px must be non-negative")
    prediction_grace_ms = rule_source["prediction_grace_ms"]
    if (
        isinstance(prediction_grace_ms, bool)
        or not isinstance(prediction_grace_ms, int)
        or prediction_grace_ms < 0
    ):
        raise EventProfileError(
            "event profile rule.prediction_grace_ms must be a non-negative integer"
        )

    return {
        "event_profile_schema_version": EVENT_PROFILE_SCHEMA_VERSION,
        "camera_id": camera_id,
        "enabled": source["enabled"],
        "rule": {
            "rule_id": rule_id.strip(),
            "type": "region_intrusion",
            "class_ids": list(class_ids),
            "region": {
                "type": "rectangle",
                "x1": round(region["x1"], 6),
                "y1": round(region["y1"], 6),
                "x2": round(region["x2"], 6),
                "y2": round(region["y2"], 6),
            },
            "dwell_ms": dwell_ms,
            "boundary_hysteresis_px": hysteresis,
            "prediction_grace_ms": prediction_grace_ms,
        },
    }


def load_event_profile(
    path: Union[Path, str], expected_camera_id: Optional[str] = None
) -> Dict[str, Any]:
    profile_path = Path(path)
    if not profile_path.exists():
        raise EventProfileError(f"event profile does not exist: {profile_path}")
    if not profile_path.is_file():
        raise EventProfileError(f"event profile is not a file: {profile_path}")
    try:
        with profile_path.open("r", encoding="utf-8") as stream:
            source = json.load(stream)
    except (OSError, ValueError) as exc:
        raise EventProfileError(f"cannot load event profile {profile_path}: {exc}") from exc
    if not isinstance(source, Mapping):
        raise EventProfileError("event profile root must be an object")
    return resolve_event_profile(source, expected_camera_id)
