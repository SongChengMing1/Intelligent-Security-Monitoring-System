from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Dict, List

from PySide6.QtCore import QObject, Signal

from .config import ViewerConfig

_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


@dataclass
class StreamSnapshot:
    status: Dict[str, Any] = field(default_factory=dict)
    detections: Dict[str, Any] = field(default_factory=dict)
    tracks: Dict[str, Any] = field(default_factory=dict)
    events: Dict[str, Any] = field(default_factory=dict)
    errors: List[str] = field(default_factory=list)
    tracking_errors: List[str] = field(default_factory=list)
    event_errors: List[str] = field(default_factory=list)


@dataclass
class StreamFrame:
    jpeg: bytes
    width: int = 0
    height: int = 0


class RtspStreamWorker(QObject):
    frame_ready = Signal(object)
    error = Signal(str)
    stopped = Signal()

    def __init__(self, url: str, timeout: float = 8.0, jpeg_quality: int = 80) -> None:
        super().__init__()
        self.url = url
        self.timeout = timeout
        self.jpeg_quality = max(1, min(100, jpeg_quality))
        self._stop_requested = False

    def stop(self) -> None:
        self._stop_requested = True

    def run(self) -> None:
        try:
            import cv2
        except ImportError as exc:
            self.error.emit(f"RTSP playback requires python OpenCV: {exc}")
            self.stopped.emit()
            return

        capture = cv2.VideoCapture()
        if hasattr(cv2, "CAP_PROP_BUFFERSIZE"):
            capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        deadline = time.monotonic() + self.timeout
        while not self._stop_requested and not capture.isOpened():
            if capture.open(self.url):
                break
            if time.monotonic() >= deadline:
                self.error.emit(f"RTSP open failed: {self.url}")
                capture.release()
                self.stopped.emit()
                return
            time.sleep(0.2)

        encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), self.jpeg_quality]
        try:
            while not self._stop_requested:
                ok, image = capture.read()
                if not ok or image is None:
                    if self._stop_requested:
                        break
                    self.error.emit("RTSP read failed")
                    break
                height, width = image.shape[:2]
                ok, encoded = cv2.imencode(".jpg", image, encode_params)
                if not ok:
                    self.error.emit("RTSP frame JPEG encode failed")
                    continue
                self.frame_ready.emit(
                    StreamFrame(
                        jpeg=encoded.tobytes(),
                        width=width,
                        height=height,
                    )
                )
        finally:
            capture.release()
            self.stopped.emit()


def stream_base_url(config: ViewerConfig) -> str:
    return f"http://{config.board_host}:{config.stream_port}"


def rtsp_url(config: ViewerConfig) -> str:
    path = config.rtsp_path if config.rtsp_path.startswith("/") else f"/{config.rtsp_path}"
    return f"rtsp://{config.board_host}:{config.rtsp_port}{path}"


def fetch_stream_snapshot(config: ViewerConfig, timeout: float = 0.8) -> StreamSnapshot:
    snapshot = StreamSnapshot()
    for endpoint, key, error_key in (
        ("status.json", "status", "errors"),
        ("detections.json", "detections", "errors"),
        ("tracks.json", "tracks", "tracking_errors"),
        ("events.json", "events", "event_errors"),
    ):
        url = f"{stream_base_url(config)}/{endpoint}"
        try:
            with _DIRECT_OPENER.open(url, timeout=timeout) as response:
                payload = response.read().decode("utf-8")
            setattr(snapshot, key, json.loads(payload))
        except (urllib.error.URLError, OSError, json.JSONDecodeError) as exc:
            if error_key == "tracking_errors":
                errors = snapshot.tracking_errors
            elif error_key == "event_errors":
                errors = snapshot.event_errors
            else:
                errors = snapshot.errors
            errors.append(f"{endpoint} unavailable: {exc}")
    return snapshot


def detection_age_ms(detections: Dict[str, Any]) -> float:
    update_time_ms = detections.get("update_time_ms")
    if isinstance(update_time_ms, (int, float)) and update_time_ms > 0:
        return time.time() * 1000.0 - float(update_time_ms)
    return 0.0
