from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional


@dataclass
class OverlaySelection:
    source: str = "none"
    objects: List[Dict[str, Any]] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)
    age_ms: float = 0.0
    reason: str = "no metadata"


def metadata_age_ms(metadata: Dict[str, Any], now_ms: Optional[float] = None) -> float:
    update_time_ms = metadata.get("update_time_ms")
    if isinstance(update_time_ms, (int, float)) and update_time_ms > 0:
        current_ms = time.time() * 1000.0 if now_ms is None else now_ms
        return max(0.0, current_ms - float(update_time_ms))
    return 0.0


def select_overlay(
    status: Dict[str, Any],
    tracks: Dict[str, Any],
    detections: Dict[str, Any],
    stale_threshold_ms: int,
    now_ms: Optional[float] = None,
) -> OverlaySelection:
    tracker_enabled = status.get("tracker_enabled") is True
    tracker_running = status.get("tracker_state") == "running"
    tracks_running = tracks.get("state") == "running"
    track_age = metadata_age_ms(tracks, now_ms)
    track_objects = _objects(tracks)
    confirmed_tracks = [obj for obj in track_objects if obj.get("track_state") == "confirmed"]

    if (
        tracker_enabled
        and tracker_running
        and tracks_running
        and confirmed_tracks
        and track_age <= stale_threshold_ms
    ):
        return OverlaySelection(
            source="tracks",
            objects=confirmed_tracks,
            metadata=tracks,
            age_ms=track_age,
            reason="confirmed tracks",
        )

    if tracker_enabled and tracker_running and tracks_running:
        reason = "tracking metadata stale" if track_age > stale_threshold_ms else "no confirmed tracks"
        return OverlaySelection(
            source="none",
            metadata=tracks,
            age_ms=track_age,
            reason=reason,
        )

    detection_age = metadata_age_ms(detections, now_ms)
    detection_objects = _objects(detections)
    if detection_objects and detection_age <= stale_threshold_ms:
        if not tracker_enabled:
            reason = "tracking disabled"
        elif not tracker_running or not tracks_running:
            reason = "tracking unavailable"
        elif not confirmed_tracks:
            reason = "no confirmed tracks"
        else:
            reason = "tracking metadata stale"
        return OverlaySelection(
            source="detections",
            objects=detection_objects,
            metadata=detections,
            age_ms=detection_age,
            reason=reason,
        )

    reason = "metadata stale" if track_age > stale_threshold_ms or detection_age > stale_threshold_ms else "no objects"
    return OverlaySelection(age_ms=max(track_age, detection_age), reason=reason)


def _objects(metadata: Dict[str, Any]) -> List[Dict[str, Any]]:
    objects = metadata.get("objects")
    if not isinstance(objects, list):
        return []
    return [obj for obj in objects if isinstance(obj, dict)]
