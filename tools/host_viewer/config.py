from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from .recording import RecordingPlan
from .event_profile import resolve_event_profile
from .tracking_profile import resolve_tracking_profile

SUPPORTED_PREPROCESS_MODES = ("resize", "letterbox")
SUPPORTED_DISPLAY_MODES = ("none", "fakesink")


def validate_preprocess_mode(value: str) -> str:
    if value not in SUPPORTED_PREPROCESS_MODES:
        expected = ", ".join(SUPPORTED_PREPROCESS_MODES)
        raise ValueError(f"unsupported preprocess_mode '{value}', expected one of: {expected}")
    return value


def validate_display_mode(value: str) -> str:
    if value not in SUPPORTED_DISPLAY_MODES:
        expected = ", ".join(SUPPORTED_DISPLAY_MODES)
        raise ValueError(f"unsupported display_mode '{value}', expected one of: {expected}")
    return value


@dataclass
class ViewerConfig:
    board_host: str = "board.local"
    board_user: str = "root"
    remote_project_dir: str = "/root/project/rknn_detect"
    stream_executable: str = "./rknn_detect_stream_server"
    stream_port: int = 18080
    rtsp_port: int = 8554
    rtsp_path: str = "/rknn_detect"
    rtsp_fps: int = 25
    h264_bitrate: int = 2000000
    display_mode: str = "none"
    output_delay_ms: int = 250
    latest_hold_ms: int = 300
    frame_ring_size: int = 60
    inference_fps: int = 10
    max_seconds: int = 0
    stale_threshold_ms: int = 1000
    preprocess_mode: str = "letterbox"
    model_path: str = "model/RK356X/yolov6n-640-640.rknn"
    video_node: str = "/dev/video0"
    width: int = 640
    height: int = 480
    fps: int = 30
    pixel_format: str = "NV12"
    box_thresh: float = 0.50
    nms_thresh: float = 0.60
    remote_output_dir: str = "/tmp/rknn_detect_host_viewer"
    ssh_connect_timeout: int = 5
    tracking_profile: Optional[Dict[str, Any]] = None
    event_profile_path: Optional[Path] = Path("profiles/events/camera0.json")
    event_profile: Optional[Dict[str, Any]] = None
    recording_plan: Optional[RecordingPlan] = None
    tracking_profiles_dir: Path = Path("profiles/tracking")
    camera_regions_dir: Path = Path("profiles/cameras")
    camera_id: str = "camera0"
    replay_executable: str = "tracking_replay"

    def __post_init__(self) -> None:
        self.preprocess_mode = validate_preprocess_mode(self.preprocess_mode)
        self.display_mode = validate_display_mode(self.display_mode)
        if self.tracking_profile is not None:
            self.tracking_profile = resolve_tracking_profile(self.tracking_profile)
        if self.event_profile is not None:
            self.event_profile = resolve_event_profile(self.event_profile, self.camera_id)

    @property
    def ssh_target(self) -> str:
        return f"{self.board_user}@{self.board_host}"

    @property
    def stream_pid_path(self) -> str:
        return f"{self.remote_output_dir}/stream_server.pid"

    @property
    def stream_log_path(self) -> str:
        return f"{self.remote_output_dir}/stream_server.log"

    def ssh_base_args(self) -> List[str]:
        return [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            f"ConnectTimeout={self.ssh_connect_timeout}",
            self.ssh_target,
        ]

    def scp_base_args(self) -> List[str]:
        return [
            "scp",
            "-q",
            "-o",
            "BatchMode=yes",
            "-o",
            f"ConnectTimeout={self.ssh_connect_timeout}",
        ]

    def with_overrides(
        self,
        *,
        board_host: Optional[str] = None,
        board_user: Optional[str] = None,
        remote_project_dir: Optional[str] = None,
        preprocess_mode: Optional[str] = None,
        model_path: Optional[str] = None,
        video_node: Optional[str] = None,
        width: Optional[int] = None,
        height: Optional[int] = None,
        fps: Optional[int] = None,
        stream_port: Optional[int] = None,
        rtsp_port: Optional[int] = None,
        rtsp_path: Optional[str] = None,
        rtsp_fps: Optional[int] = None,
        h264_bitrate: Optional[int] = None,
        display_mode: Optional[str] = None,
        output_delay_ms: Optional[int] = None,
        latest_hold_ms: Optional[int] = None,
        frame_ring_size: Optional[int] = None,
        inference_fps: Optional[int] = None,
        max_seconds: Optional[int] = None,
        stale_threshold_ms: Optional[int] = None,
        remote_output_dir: Optional[str] = None,
        tracking_profile: Optional[Dict[str, Any]] = None,
        event_profile_path: Optional[Path] = None,
        recording_plan: Optional[RecordingPlan] = None,
        tracking_profiles_dir: Optional[Path] = None,
        camera_regions_dir: Optional[Path] = None,
        camera_id: Optional[str] = None,
    ) -> "ViewerConfig":
        values = self.__dict__.copy()
        overrides = {
            "board_host": board_host,
            "board_user": board_user,
            "remote_project_dir": remote_project_dir,
            "preprocess_mode": preprocess_mode,
            "model_path": model_path,
            "video_node": video_node,
            "width": width,
            "height": height,
            "fps": fps,
            "stream_port": stream_port,
            "rtsp_port": rtsp_port,
            "rtsp_path": rtsp_path,
            "rtsp_fps": rtsp_fps,
            "h264_bitrate": h264_bitrate,
            "display_mode": display_mode,
            "output_delay_ms": output_delay_ms,
            "latest_hold_ms": latest_hold_ms,
            "frame_ring_size": frame_ring_size,
            "inference_fps": inference_fps,
            "max_seconds": max_seconds,
            "stale_threshold_ms": stale_threshold_ms,
            "remote_output_dir": remote_output_dir,
            "tracking_profile": tracking_profile,
            "event_profile_path": event_profile_path,
            "recording_plan": recording_plan,
            "tracking_profiles_dir": tracking_profiles_dir,
            "camera_regions_dir": camera_regions_dir,
            "camera_id": camera_id,
        }
        for key, value in overrides.items():
            if value is not None:
                values[key] = value
        return ViewerConfig(**values)
