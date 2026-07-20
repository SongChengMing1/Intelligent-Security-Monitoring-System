from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple, Union


REPLAY_MODES = ("recorded", "tracker-rerun", "policy-tracker-rerun")
REPLAY_LAYERS = ("high-pass", "low-only", "rejected", "tracker-input", "recorded-tracks")
REPLAY_RERUN_LAYERS = REPLAY_LAYERS + ("replayed-tracks",)
IMAGE_STATES = ("none", "exact", "unavailable", "dropped")


class ReplayError(ValueError):
    pass


@dataclass
class ReplaySession:
    path: Path
    manifest: Dict[str, Any]
    records: List[Dict[str, Any]] = field(default_factory=list)
    truncated_final_line: bool = False

    @property
    def width(self) -> int:
        return int(self.manifest.get("source_width", 0))

    @property
    def height(self) -> int:
        return int(self.manifest.get("source_height", 0))

    @property
    def profile_id(self) -> str:
        return str(self.manifest.get("effective_profile_id", "-"))

    @property
    def profile_hash(self) -> str:
        return str(self.manifest.get("effective_profile_hash", "-"))


def load_replay_session(path: Union[Path, str]) -> ReplaySession:
    session_path = Path(path)
    if not session_path.is_dir():
        raise ReplayError(f"Replay Session must be a local directory: {session_path}")
    try:
        manifest = json.loads((session_path / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReplayError(f"manifest read failed: {exc}") from exc
    if manifest.get("manifest_schema_version") != 1 or manifest.get("record_schema_version") != 1:
        raise ReplayError("unsupported recording manifest schema")
    records: List[Dict[str, Any]] = []
    truncated = False
    segments = sorted(session_path.glob("records-*.jsonl"))
    for segment_index, segment in enumerate(segments):
        text = segment.read_text(encoding="utf-8")
        lines = text.splitlines(keepends=True)
        for line_index, line in enumerate(lines):
            if not line.endswith("\n"):
                if segment_index != len(segments) - 1 or line_index != len(lines) - 1:
                    raise ReplayError(f"truncated non-final record in {segment}")
                truncated = True
                continue
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ReplayError(f"corrupt record in {segment}: {exc}") from exc
            if record.get("record_schema_version") != 1:
                raise ReplayError("unsupported record schema")
            records.append(record)
    if truncated:
        raise ReplayError("session contains a truncated final record")
    if not records:
        raise ReplayError("session contains no complete records")
    return ReplaySession(session_path, manifest, records, truncated)


def image_path(session: ReplaySession, record: Dict[str, Any]) -> Optional[Path]:
    image = record.get("image", {})
    if image.get("state") != "exact":
        return None
    if image.get("capture_frame_id") != record.get("capture_frame_id"):
        return None
    relative = image.get("relative_path")
    if not isinstance(relative, str) or not relative:
        return None
    candidate = (session.path / relative).resolve()
    try:
        candidate.relative_to(session.path.resolve())
    except ValueError:
        return None
    return candidate if candidate.exists() else None


def blank_canvas_size(session: ReplaySession) -> Tuple[int, int]:
    return max(1, session.width), max(1, session.height)


def observations_for_layer(record: Dict[str, Any], layer: str) -> List[Dict[str, Any]]:
    observations = [item for item in record.get("observations", []) if isinstance(item, dict)]
    if layer == "high-pass":
        return [item for item in observations if item.get("origin") == "high_pass"]
    if layer == "low-only":
        return [item for item in observations if item.get("origin") == "low_only"]
    if layer == "rejected":
        return [item for item in observations if not item.get("admitted")]
    if layer == "tracker-input":
        ids = set(record.get("tracker_input_observation_ids", []))
        return [item for item in observations if item.get("observation_id") in ids]
    if layer == "recorded-tracks":
        return [item for item in record.get("recorded_tracks", {}).get("objects", []) if isinstance(item, dict)]
    if layer == "replayed-tracks":
        return [item for item in record.get("replayed_tracks", {}).get("objects", []) if isinstance(item, dict)]
    return []


def validate_replay_profile(
    session: ReplaySession, resolved_profile: Dict[str, Any], mode: str
) -> None:
    if mode not in REPLAY_MODES:
        raise ReplayError(f"unsupported replay mode: {mode}")
    if mode == "recorded":
        return
    source_profile = session.manifest.get("resolved_profile")
    if not isinstance(source_profile, dict):
        raise ReplayError("Replay Session has no resolved source Profile")
    source_detector = source_profile.get("detector", {})
    selected_detector = resolved_profile.get("detector", {})
    for key in ("high_threshold", "nms_threshold"):
        try:
            source_value = float(source_detector[key])
            selected_value = float(selected_detector[key])
        except (KeyError, TypeError, ValueError) as exc:
            raise ReplayError(f"Replay detector Profile is incomplete: {key}") from exc
        if abs(source_value - selected_value) > 1e-6:
            raise ReplayError(
                f"Replay detector {key} mismatch: source={source_value:g}, selected={selected_value:g}"
            )


class ReplayWorkspace:
    """Local Replay lifecycle: load paused, play/pause, step, and unload."""

    def __init__(self) -> None:
        self.controller: Optional[ReplayController] = None
        self.state = "empty"

    def load(self, session: ReplaySession, mode: str = "recorded") -> ReplayController:
        if mode not in REPLAY_MODES:
            raise ReplayError(f"unsupported replay mode: {mode}")
        self.controller = ReplayController(session)
        self.controller.mode = mode
        self.controller.playing = False
        self.state = "paused"
        return self.controller

    def play(self) -> None:
        if self.controller is None:
            raise ReplayError("Replay Session is not loaded")
        self.controller.playing = True
        self.state = "playing"

    def pause(self) -> None:
        if self.controller is not None:
            self.controller.playing = False
            self.state = "paused"

    def step(self, delta: int) -> Dict[str, Any]:
        if self.controller is None:
            raise ReplayError("Replay Session is not loaded")
        self.controller.playing = False
        self.state = "paused"
        return self.controller.step(delta)

    def stop(self) -> None:
        if self.controller is not None:
            self.controller.playing = False
        self.controller = None
        self.state = "empty"


def run_replay_command(executable: Union[Path, str], session: ReplaySession, mode: str,
                       resolved_profile: Optional[Union[Path, str]] = None,
                       output: Optional[Union[Path, str]] = None,
                       allow_fixed_detector_mismatch: bool = False) -> subprocess.CompletedProcess[str]:
    if mode not in REPLAY_MODES:
        raise ReplayError(f"unsupported replay mode: {mode}")
    command = [str(executable), "--session", str(session.path), "--mode", mode]
    if resolved_profile:
        command.extend(["--resolved-profile", str(resolved_profile)])
    if output:
        command.extend(["--output", str(output)])
    if allow_fixed_detector_mismatch:
        command.append("--allow-fixed-detector-mismatch")
    return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)


class ReplayController:
    def __init__(self, session: ReplaySession) -> None:
        self.session = session
        self.index = 0
        self.speed = 1.0
        self.playing = False
        self.mode = "recorded"
        self.show_layers = set(REPLAY_LAYERS)

    @property
    def record(self) -> Dict[str, Any]:
        return self.session.records[self.index]

    def step(self, delta: int) -> Dict[str, Any]:
        if self.session.records:
            self.index = max(0, min(len(self.session.records) - 1, self.index + delta))
        return self.record

    def next_interval_ms(self) -> int:
        if not self.session.records or len(self.session.records) == 1:
            return 1000
        current = self.record.get("capture_timestamp_ms", 0)
        following = self.session.records[min(self.index + 1, len(self.session.records) - 1)].get("capture_timestamp_ms", current)
        return max(1, int(max(1, following - current) / max(0.01, self.speed)))

    def load_rerun_output(self, output_path: Union[Path, str]) -> None:
        output = Path(output_path)
        tracks_path = output / "tracks.jsonl"
        if not tracks_path.exists():
            raise ReplayError("replay tracks output is missing")
        by_frame: Dict[int, Dict[str, Any]] = {}
        for line in tracks_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                value = json.loads(line)
                by_frame[int(value.get("capture_frame_id", 0))] = value
        policy_by_frame: Dict[int, List[int]] = {}
        decisions_path = output / "policy-decisions.jsonl"
        if self.mode == "policy-tracker-rerun":
            if not decisions_path.exists():
                raise ReplayError("policy replay decisions output is missing")
            for line in decisions_path.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                value = json.loads(line)
                policy_by_frame[int(value.get("capture_frame_id", 0))] = [
                    int(item) for item in value.get("admitted_observation_ids", [])
                ]
        for record in self.session.records:
            frame_id = int(record.get("capture_frame_id", 0))
            frame = by_frame.get(frame_id)
            if frame is not None:
                record["replayed_tracks"] = frame
            if self.mode == "policy-tracker-rerun" and frame_id in policy_by_frame:
                admitted_ids = set(policy_by_frame[frame_id])
                observations = record.get("observations", [])
                for observation in observations:
                    if isinstance(observation, dict):
                        observation["admitted"] = int(observation.get("observation_id", 0)) in admitted_ids
                record["tracker_input_observation_ids"] = policy_by_frame[frame_id]
