from __future__ import annotations

import hashlib
import json
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict, List, Mapping, Union


PROFILE_SCHEMA_VERSION = 1

DEFAULT_RESOLVED_PROFILE: Dict[str, Any] = {
    "profile_schema_version": PROFILE_SCHEMA_VERSION,
    "profile_id": "default-general",
    "detector": {
        "high_threshold": 0.5,
        "nms_threshold": 0.6,
    },
    "observation": {
        "allowed_class_ids": [],
        "edge_margin": 0.0,
        "max_observations": 256,
        "min_area": 0.0,
        "min_height": 0.0,
        "min_width": 0.0,
        "roi": {
            "enabled": False,
            "x1": 0.0,
            "x2": 1.0,
            "y1": 0.0,
            "y2": 1.0,
        },
    },
    "tracker": {
        "confirm_hits": 2,
        "high_threshold": 0.5,
        "lost_timeout_ms": 1000,
        "match_threshold": 0.8,
        "max_tracks": 64,
        "new_track_threshold": 0.5,
        "low_threshold": 0.35,
        "second_match_threshold": 0.5,
        "type": "bytetrack",
    },
}


class TrackingProfileError(ValueError):
    pass


def _reject_unknown(source: Mapping[str, Any], allowed: set[str], field: str) -> None:
    unknown = sorted(set(source) - allowed)
    if unknown:
        raise TrackingProfileError(f"{field} contains unknown field: {unknown[0]}")


def _merge_object(target: Dict[str, Any], source: Mapping[str, Any], field: str) -> None:
    if not isinstance(source, Mapping):
        raise TrackingProfileError(f"{field} must be an object")
    _reject_unknown(source, set(target), field)
    for key, value in source.items():
        if isinstance(target.get(key), dict):
            _merge_object(target[key], value, f"{field}.{key}")
        else:
            target[key] = value


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TrackingProfileError(f"{field} must be a number")
    return float(value)


def validate_resolved_profile(profile: Mapping[str, Any]) -> None:
    if profile["profile_schema_version"] != PROFILE_SCHEMA_VERSION:
        raise TrackingProfileError("profile_schema_version must be 1")
    if not isinstance(profile["profile_id"], str) or not profile["profile_id"]:
        raise TrackingProfileError("profile_id must not be empty")

    detector = profile["detector"]
    high = _number(detector["high_threshold"], "detector.high_threshold")
    nms = _number(detector["nms_threshold"], "detector.nms_threshold")
    if not 0.0 <= high <= 1.0:
        raise TrackingProfileError("detector.high_threshold must be in [0,1]")
    if not 0.0 < nms <= 1.0:
        raise TrackingProfileError("detector.nms_threshold must be in (0,1]")

    observation = profile["observation"]
    class_ids = observation["allowed_class_ids"]
    if not isinstance(class_ids, list) or any(isinstance(item, bool) or not isinstance(item, int) for item in class_ids):
        raise TrackingProfileError("observation.allowed_class_ids must be an integer array")
    if any(item < 0 for item in class_ids):
        raise TrackingProfileError("observation.allowed_class_ids must be non-negative")
    if len(set(class_ids)) != len(class_ids):
        raise TrackingProfileError("observation.allowed_class_ids must be unique")
    for key in ("min_width", "min_height", "min_area", "edge_margin"):
        if _number(observation[key], f"observation.{key}") < 0.0:
            raise TrackingProfileError(f"observation.{key} must be non-negative")
    max_observations = observation["max_observations"]
    if isinstance(max_observations, bool) or not isinstance(max_observations, int) or not 1 <= max_observations <= 4096:
        raise TrackingProfileError("observation.max_observations must be in [1,4096]")
    roi = observation["roi"]
    if not isinstance(roi["enabled"], bool):
        raise TrackingProfileError("observation.roi.enabled must be boolean")
    x1 = _number(roi["x1"], "observation.roi.x1")
    x2 = _number(roi["x2"], "observation.roi.x2")
    y1 = _number(roi["y1"], "observation.roi.y1")
    y2 = _number(roi["y2"], "observation.roi.y2")
    if not (0.0 <= x1 < x2 <= 1.0 and 0.0 <= y1 < y2 <= 1.0):
        raise TrackingProfileError("observation.roi must satisfy normalized ordered bounds")

    tracker = profile["tracker"]
    if tracker["type"] != "bytetrack":
        raise TrackingProfileError("tracker.type must be bytetrack")
    low = _number(tracker["low_threshold"], "tracker.low_threshold")
    tracker_high = _number(tracker["high_threshold"], "tracker.high_threshold")
    new_track = _number(tracker["new_track_threshold"], "tracker.new_track_threshold")
    if not 0.0 <= low <= tracker_high <= new_track <= 1.0:
        raise TrackingProfileError("tracker thresholds must satisfy 0 <= low <= high <= new_track <= 1")
    for key in ("match_threshold", "second_match_threshold"):
        value = _number(tracker[key], f"tracker.{key}")
        if not 0.0 < value <= 1.0:
            raise TrackingProfileError(f"tracker.{key} must be in (0,1]")
    for key, maximum in (("confirm_hits", None), ("lost_timeout_ms", None), ("max_tracks", 4096)):
        value = tracker[key]
        if isinstance(value, bool) or not isinstance(value, int) or value < 1 or (maximum and value > maximum):
            suffix = f"[1,{maximum}]" if maximum else "positive"
            raise TrackingProfileError(f"tracker.{key} must be {suffix}")


def resolve_tracking_profile(source: Mapping[str, Any]) -> Dict[str, Any]:
    if not isinstance(source, Mapping):
        raise TrackingProfileError("profile root must be an object")
    allowed = {"profile_schema_version", "profile_id", "description", "detector", "observation", "tracker"}
    _reject_unknown(source, allowed, "profile")
    algorithm_source = {key: value for key, value in source.items() if key != "description"}
    resolved = deepcopy(DEFAULT_RESOLVED_PROFILE)
    _merge_object(resolved, algorithm_source, "profile")
    validate_resolved_profile(resolved)
    for key in ("high_threshold", "nms_threshold"):
        resolved["detector"][key] = round(float(resolved["detector"][key]), 6)
    for key in ("min_width", "min_height", "min_area", "edge_margin"):
        resolved["observation"][key] = round(float(resolved["observation"][key]), 6)
    for key in ("x1", "x2", "y1", "y2"):
        resolved["observation"]["roi"][key] = round(float(resolved["observation"]["roi"][key]), 6)
    for key in ("low_threshold", "high_threshold", "new_track_threshold", "match_threshold", "second_match_threshold"):
        resolved["tracker"][key] = round(float(resolved["tracker"][key]), 6)
    return resolved


def load_tracking_profile(path: Union[Path, str]) -> Dict[str, Any]:
    with Path(path).open("r", encoding="utf-8") as stream:
        source = json.load(stream)
    return resolve_tracking_profile(source)


def canonical_profile_json(resolved: Mapping[str, Any]) -> str:
    validate_resolved_profile(resolved)
    return json.dumps(resolved, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":"))


def profile_hash(resolved: Mapping[str, Any]) -> str:
    payload = canonical_profile_json(resolved).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def effective_profile_mismatches(status: Mapping[str, Any], resolved: Mapping[str, Any]) -> List[str]:
    """Return operator-facing differences between requested and board-effective Profile identity."""
    mismatches: List[str] = []
    expected_hash = profile_hash(resolved)
    if status.get("profile_id") != resolved["profile_id"]:
        mismatches.append(f"profile_id expected {resolved['profile_id']!r}, got {status.get('profile_id')!r}")
    if status.get("profile_hash") != expected_hash:
        mismatches.append(f"profile_hash expected {expected_hash!r}, got {status.get('profile_hash')!r}")
    return mismatches
