"""Host Viewer live-session lifecycle.

The GUI owns presentation and scheduling; this module owns the state contract
so it can be exercised without a Qt application or a physical board.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Optional


class LiveState(str, Enum):
    IDLE = "Idle"
    STARTING = "Starting"
    RUNNING = "Running"
    DEGRADED = "Degraded"
    STOPPING = "Stopping"
    ERROR = "Error"


@dataclass(frozen=True)
class LiveError:
    code: str
    message: str


@dataclass(frozen=True)
class LiveEvidence:
    """The small set of observations needed to declare a usable Live run."""

    board_status_readable: bool = False
    rtsp_frame_received: bool = False
    video_frame_id: int = 0
    detection_frame_id: int = 0
    alignment_state: str = ""
    process_alive: bool = True
    transient_error: str = ""
    fatal_error: str = ""
    side_channel_metadata_ready: bool = False

    @property
    def metadata_aligned(self) -> bool:
        if self.detection_frame_id <= 0:
            return False
        if self.alignment_state == "clean":
            # The production clean RTSP branch intentionally carries no
            # detector overlay.  It is ready only when its current AI-side
            # metadata has been observed separately.
            return self.side_channel_metadata_ready
        if self.alignment_state:
            return self.alignment_state == "matched"
        return self.video_frame_id > 0 and self.video_frame_id == self.detection_frame_id

    @property
    def ready(self) -> bool:
        return (
            self.board_status_readable
            and self.rtsp_frame_received
            and self.metadata_aligned
            and not self.transient_error
        )


class LiveWorkflow:
    """Explicit Live state machine with optional synchronous adapters.

    ``begin_start``/``complete_start`` and ``request_stop``/``complete_stop``
    are the asynchronous boundary used by the GUI.  ``start`` and ``stop``
    are convenience methods for tests and small headless callers that can run
    their adapters synchronously.
    """

    def __init__(self, adapter: Optional[Any] = None) -> None:
        self.adapter = adapter
        self.state = LiveState.IDLE
        self.error: Optional[LiveError] = None
        self.diagnostic = ""
        self._next_token = 0
        self._active_token: Optional[int] = None

    def begin_start(self) -> int:
        if self.state not in {LiveState.IDLE, LiveState.ERROR}:
            raise RuntimeError(f"cannot start Live from {self.state.value}")
        self._next_token += 1
        self._active_token = self._next_token
        self.state = LiveState.STARTING
        self.error = None
        self.diagnostic = ""
        return self._next_token

    def complete_start(
        self,
        token: int,
        message: Optional[str] = None,
        *,
        code: str = "launch",
    ) -> bool:
        """Complete the launch part, ignoring a result from a cancelled start."""

        if token != self._active_token or self.state != LiveState.STARTING:
            return False
        if message:
            self.state = LiveState.ERROR
            self.error = LiveError(code, message)
            self._active_token = None
            return False
        return True

    def start(self) -> int:
        token = self.begin_start()
        if self.adapter is None:
            return token
        try:
            preflight = self.adapter.preflight()
            if preflight is False:
                raise RuntimeError("Live preflight failed")
        except Exception as exc:
            self.complete_start(token, str(exc), code="preflight")
            return token
        try:
            started = self.adapter.start()
            if started is False:
                raise RuntimeError("Live launch failed")
        except Exception as exc:
            self.complete_start(token, str(exc), code="launch")
        return token

    def observe(self, evidence: LiveEvidence) -> LiveState:
        if self.state not in {LiveState.STARTING, LiveState.RUNNING, LiveState.DEGRADED}:
            return self.state

        if evidence.fatal_error or not evidence.process_alive:
            message = evidence.fatal_error or "Live process exited"
            self.state = LiveState.ERROR
            self.error = LiveError("process-exit" if not evidence.process_alive else "runtime", message)
            self._active_token = None
            return self.state

        self.diagnostic = evidence.transient_error
        if evidence.ready:
            self.state = LiveState.RUNNING
            self.diagnostic = ""
        elif evidence.transient_error or self.state != LiveState.STARTING:
            self.state = LiveState.DEGRADED
        return self.state

    def request_stop(self) -> LiveState:
        if self.state == LiveState.IDLE:
            return self.state
        if self.state != LiveState.STOPPING:
            self.state = LiveState.STOPPING
            self._active_token = None
        return self.state

    def stop(self) -> LiveState:
        state = self.request_stop()
        if state != LiveState.STOPPING or self.adapter is None:
            return state
        try:
            stopped = self.adapter.stop()
            if stopped is False:
                raise RuntimeError("Live stop failed")
        except Exception as exc:
            self.complete_stop(str(exc))
        return self.state

    def complete_stop(self, message: Optional[str] = None) -> bool:
        if self.state != LiveState.STOPPING:
            return False
        if message:
            self.state = LiveState.ERROR
            self.error = LiveError("stop", message)
            return False
        self.state = LiveState.IDLE
        self.error = None
        self.diagnostic = ""
        self._active_token = None
        return True
