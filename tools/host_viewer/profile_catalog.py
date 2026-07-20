from __future__ import annotations

import json
from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Union

from .tracking_profile import (
    TrackingProfileError,
    profile_hash,
    resolve_tracking_profile,
    validate_resolved_profile,
)


class ProfileCatalogError(ValueError):
    """Raised when a scene Profile catalog cannot be loaded safely."""


class CameraRegionError(ValueError):
    """Raised when a logical camera region file is invalid."""


@dataclass(frozen=True)
class TrackingProfilePreset:
    profile_id: str
    description: str
    source_path: Path
    resolved: Dict[str, Any]

    @property
    def profile_hash(self) -> str:
        return profile_hash(self.resolved)


@dataclass(frozen=True)
class CameraRegion:
    camera_id: str
    description: str
    roi: Dict[str, Any]
    source_path: Path


def _load_json(path: Path, error_type: type[ValueError], label: str) -> Mapping[str, Any]:
    if not path.exists():
        raise error_type(f"{label} does not exist: {path}")
    if not path.is_file():
        raise error_type(f"{label} is not a file: {path}")
    try:
        with path.open("r", encoding="utf-8") as stream:
            source = json.load(stream)
    except (OSError, ValueError) as exc:
        raise error_type(f"cannot load {label} {path}: {exc}") from exc
    if not isinstance(source, Mapping):
        raise error_type(f"{label} root must be an object: {path}")
    return source


def discover_tracking_profiles(directory: Union[Path, str]) -> List[TrackingProfilePreset]:
    root = Path(directory)
    if not root.exists() or not root.is_dir():
        raise ProfileCatalogError(f"tracking profile directory does not exist: {root}")
    paths = sorted(root.glob("*.json"))
    if not paths:
        raise ProfileCatalogError(f"tracking profile directory contains no JSON files: {root}")

    presets: List[TrackingProfilePreset] = []
    seen_ids: set[str] = set()
    for path in paths:
        source = _load_json(path, ProfileCatalogError, "tracking Profile")
        description = source.get("description")
        if not isinstance(description, str) or not description.strip():
            raise ProfileCatalogError(f"tracking Profile description must be non-empty: {path}")
        try:
            resolved = resolve_tracking_profile(source)
        except TrackingProfileError as exc:
            raise ProfileCatalogError(f"invalid tracking Profile {path}: {exc}") from exc
        profile_id = resolved["profile_id"]
        if profile_id in seen_ids:
            raise ProfileCatalogError(f"duplicate profile_id {profile_id!r} in tracking Profile catalog")
        seen_ids.add(profile_id)
        presets.append(TrackingProfilePreset(profile_id, description.strip(), path, resolved))
    return sorted(presets, key=lambda preset: preset.profile_id)


def _validate_roi(source: Mapping[str, Any], field: str) -> Dict[str, Any]:
    allowed = {"enabled", "x1", "y1", "x2", "y2"}
    unknown = sorted(set(source) - allowed)
    if unknown:
        raise CameraRegionError(f"{field} contains unknown field: {unknown[0]}")
    missing = sorted(allowed - set(source))
    if missing:
        raise CameraRegionError(f"{field} is missing field: {missing[0]}")
    if not isinstance(source["enabled"], bool):
        raise CameraRegionError(f"{field}.enabled must be boolean")
    values: Dict[str, Any] = {"enabled": source["enabled"]}
    for key in ("x1", "y1", "x2", "y2"):
        value = source[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise CameraRegionError(f"{field}.{key} must be a number")
        values[key] = round(float(value), 6)
    if not (0.0 <= values["x1"] < values["x2"] <= 1.0 and 0.0 <= values["y1"] < values["y2"] <= 1.0):
        raise CameraRegionError(f"{field} must satisfy normalized ordered bounds")
    return values


def load_camera_region(path: Union[Path, str]) -> CameraRegion:
    source = _load_json(Path(path), CameraRegionError, "Camera Region")
    allowed = {"camera_id", "description", "roi"}
    unknown = sorted(set(source) - allowed)
    if unknown:
        raise CameraRegionError(f"Camera Region contains unknown field: {unknown[0]}")
    missing = sorted(allowed - set(source))
    if missing:
        raise CameraRegionError(f"Camera Region is missing field: {missing[0]}")
    camera_id = source["camera_id"]
    description = source["description"]
    if not isinstance(camera_id, str) or not camera_id.strip():
        raise CameraRegionError("Camera Region camera_id must be non-empty")
    if not isinstance(description, str) or not description.strip():
        raise CameraRegionError("Camera Region description must be non-empty")
    roi = source["roi"]
    if not isinstance(roi, Mapping):
        raise CameraRegionError("Camera Region roi must be an object")
    return CameraRegion(camera_id.strip(), description.strip(), _validate_roi(roi, "Camera Region roi"), Path(path))


def apply_camera_region(resolved: Mapping[str, Any], region: CameraRegion) -> Dict[str, Any]:
    effective = deepcopy(dict(resolved))
    roi = deepcopy(region.roi)
    if not roi["enabled"]:
        # The board canonicalizes a disabled ROI to TrackingRoiConfig's
        # full-frame defaults before calculating the effective Profile Hash.
        roi.update({"x1": 0.0, "y1": 0.0, "x2": 1.0, "y2": 1.0})
    effective.setdefault("observation", {})["roi"] = roi
    try:
        validate_resolved_profile(effective)
    except (KeyError, TrackingProfileError) as exc:
        raise ProfileCatalogError(f"cannot apply Camera Region to resolved Profile: {exc}") from exc
    return effective


def resolve_effective_profile(
    source: Mapping[str, Any], region: Optional[CameraRegion] = None
) -> Dict[str, Any]:
    resolved = resolve_tracking_profile(source)
    return apply_camera_region(resolved, region) if region is not None else resolved
