"""Recording plans and the recorder-side lifecycle used by Host Viewer."""

from __future__ import annotations

import uuid
import re
from dataclasses import dataclass
from enum import Enum
from typing import Any, Dict, List, Mapping, Optional


RECORDING_MODES = ("off", "metadata", "sampled", "all")


class RecorderState(str, Enum):
    DISABLED = "disabled"
    STARTING = "starting"
    RECORDING = "recording"
    FINALIZING = "finalizing"
    FINALIZED = "finalized"
    LIMIT_REACHED = "limit_reached"
    FAILED = "failed"
    INTERRUPTED = "interrupted"


def _new_session_id(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex[:16]}"


def _validate_session_id(value: str, field: str) -> None:
    if not value or re.fullmatch(r"[A-Za-z0-9._-]+", value) is None:
        raise ValueError(f"{field} contains unsafe session ID characters")


@dataclass(frozen=True)
class RecordingPlan:
    mode: str
    runtime_session_id: str
    recording_session_id: str = ""
    recording_root: str = "recordings"
    jpeg_every: int = 10
    jpeg_quality: int = 80

    def __post_init__(self) -> None:
        if self.mode not in RECORDING_MODES:
            raise ValueError(f"unsupported recording mode: {self.mode}")
        if not self.runtime_session_id:
            raise ValueError("runtime_session_id must not be empty")
        _validate_session_id(self.runtime_session_id, "runtime_session_id")
        if self.mode != "off" and not self.recording_session_id:
            raise ValueError("recording_session_id must not be empty when recording is enabled")
        if self.recording_session_id:
            _validate_session_id(self.recording_session_id, "recording_session_id")
        if not isinstance(self.recording_root, str) or not self.recording_root.strip():
            raise ValueError("recording_root must not be empty")
        if self.jpeg_every < 1:
            raise ValueError("jpeg_every must be positive")
        if not 1 <= self.jpeg_quality <= 100:
            raise ValueError("jpeg_quality must be in [1,100]")

    @classmethod
    def create(
        cls,
        mode: str,
        *,
        runtime_session_id: Optional[str] = None,
        recording_session_id: Optional[str] = None,
        recording_root: str = "recordings",
        jpeg_every: int = 10,
        jpeg_quality: int = 80,
    ) -> "RecordingPlan":
        if mode not in RECORDING_MODES:
            raise ValueError(f"unsupported recording mode: {mode}")
        runtime_id = runtime_session_id or _new_session_id("runtime")
        recording_id = "" if mode == "off" else (recording_session_id or _new_session_id("recording-camera0"))
        return cls(mode, runtime_id, recording_id, recording_root, jpeg_every, jpeg_quality)

    @property
    def enabled(self) -> bool:
        return self.mode != "off"

    @property
    def frame_mode(self) -> str:
        if self.mode in {"off", "metadata"}:
            return "none"
        return self.mode

    def stream_args(self) -> List[str]:
        args = ["--runtime-session-id", self.runtime_session_id]
        if not self.enabled:
            return args
        args.extend(
            [
                "--record-analysis",
                "--recording-root",
                self.recording_root,
                "--recording-session-id",
                self.recording_session_id,
                "--record-frame-mode",
                self.frame_mode,
                "--record-jpeg-every",
                str(self.jpeg_every),
                "--record-jpeg-quality",
                str(self.jpeg_quality),
            ]
        )
        return args


class RecordingWorkflow:
    """Recorder state is observed independently from the Live state."""

    def __init__(self, plan: RecordingPlan) -> None:
        self.plan = plan
        self.state = RecorderState.DISABLED if not plan.enabled else RecorderState.STARTING
        self.error = ""
        self.stop_requested = False
        self.session_id = plan.recording_session_id

    @property
    def live_can_release(self) -> bool:
        return self.state in {
            RecorderState.DISABLED,
            RecorderState.FINALIZED,
            RecorderState.LIMIT_REACHED,
            RecorderState.FAILED,
            RecorderState.INTERRUPTED,
        }

    @property
    def terminal(self) -> bool:
        return self.live_can_release

    def observe(self, status: Mapping[str, Any]) -> RecorderState:
        raw_state = status.get("state")
        if isinstance(raw_state, str):
            try:
                self.state = RecorderState(raw_state)
            except ValueError:
                self.error = f"unknown recorder state: {raw_state}"
                self.state = RecorderState.FAILED
        session_id = status.get("session_id", status.get("recording_session_id"))
        if isinstance(session_id, str) and session_id:
            self.session_id = session_id
        error = status.get("error")
        if isinstance(error, str) and error:
            self.error = error
        return self.state

    def mark_start_failed(self, message: str) -> RecorderState:
        self.state = RecorderState.FAILED
        self.error = message
        return self.state

    def request_stop(self, *, confirmed: bool) -> bool:
        if not self.plan.enabled or not confirmed:
            return False
        self.stop_requested = True
        if self.state in {RecorderState.STARTING, RecorderState.RECORDING}:
            self.state = RecorderState.FINALIZING
        return True

    def mark_interrupted(self, message: str) -> RecorderState:
        self.state = RecorderState.INTERRUPTED
        self.error = message
        return self.state
