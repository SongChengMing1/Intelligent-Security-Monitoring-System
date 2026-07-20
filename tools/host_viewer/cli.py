from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .config import SUPPORTED_DISPLAY_MODES, ViewerConfig
from .controller import BoardController, EventLog
from .event_profile import load_event_profile


def apply_default_event_profile(config: ViewerConfig) -> None:
    """Resolve the configured default event profile before a stream launch."""

    if config.event_profile is not None or config.event_profile_path is None:
        return
    profile_path = Path(config.event_profile_path)
    if not profile_path.is_absolute():
        profile_path = Path(__file__).resolve().parents[2] / profile_path
    config.event_profile = load_event_profile(profile_path, config.camera_id)


def build_parser() -> argparse.ArgumentParser:
    default_config = ViewerConfig()
    parser = argparse.ArgumentParser(description="RK3568 rknn_detect host viewer control CLI")
    parser.add_argument("--board-host", default="board.local")
    parser.add_argument("--board-user", default="root")
    parser.add_argument("--remote-project-dir", default="/root/project/rknn_detect")
    parser.add_argument("--preprocess-mode", choices=["resize", "letterbox"], default=default_config.preprocess_mode)
    parser.add_argument("--model-path", default=default_config.model_path)
    parser.add_argument("--video-node", default="/dev/video0")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--stream-port", type=int, default=default_config.stream_port)
    parser.add_argument("--rtsp-port", type=int, default=default_config.rtsp_port)
    parser.add_argument("--rtsp-path", default=default_config.rtsp_path)
    parser.add_argument("--rtsp-fps", type=int, default=default_config.rtsp_fps)
    parser.add_argument("--h264-bitrate", type=int, default=default_config.h264_bitrate)
    parser.add_argument("--display-mode", choices=SUPPORTED_DISPLAY_MODES, default=default_config.display_mode)
    parser.add_argument("--output-delay-ms", type=int, default=default_config.output_delay_ms)
    parser.add_argument("--latest-hold-ms", type=int, default=default_config.latest_hold_ms)
    parser.add_argument("--frame-ring-size", type=int, default=default_config.frame_ring_size)
    parser.add_argument("--inference-fps", type=int, default=default_config.inference_fps)
    parser.add_argument("--stale-threshold-ms", type=int, default=default_config.stale_threshold_ms)
    parser.add_argument("--max-seconds", type=int, default=default_config.max_seconds)
    parser.add_argument("--remote-output-dir", default=default_config.remote_output_dir)
    parser.add_argument("--log-path", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="print the remote GStreamer stream command without launching")
    parser.add_argument("--check-ssh", action="store_true", help="check SSH connectivity")
    parser.add_argument("--start", action="store_true", help="start the remote GStreamer stream server")
    parser.add_argument("--wait", action="store_true", help="wait for an attached remote stream server")
    parser.add_argument("--stop", action="store_true", help="request a clean stop for the remote stream server")
    parser.add_argument("--status", action="store_true", help="check whether the remote stream server is running")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    config = ViewerConfig().with_overrides(
        board_host=args.board_host,
        board_user=args.board_user,
        remote_project_dir=args.remote_project_dir,
        preprocess_mode=args.preprocess_mode,
        model_path=args.model_path,
        video_node=args.video_node,
        width=args.width,
        height=args.height,
        fps=args.fps,
        stream_port=args.stream_port,
        rtsp_port=args.rtsp_port,
        rtsp_path=args.rtsp_path,
        rtsp_fps=args.rtsp_fps,
        h264_bitrate=args.h264_bitrate,
        display_mode=args.display_mode,
        output_delay_ms=args.output_delay_ms,
        latest_hold_ms=args.latest_hold_ms,
        frame_ring_size=args.frame_ring_size,
        inference_fps=args.inference_fps,
        max_seconds=args.max_seconds,
        stale_threshold_ms=args.stale_threshold_ms,
        remote_output_dir=args.remote_output_dir,
    )
    if args.start or args.dry_run:
        apply_default_event_profile(config)
    event_log = EventLog(args.log_path)
    controller = BoardController(config, event_log)

    if args.dry_run:
        print(controller.build_remote_stream_command())

    ok = True
    if args.check_ssh:
        ok = controller.check_ssh() and ok

    if args.status:
        ok = controller.is_remote_stream_running() and ok

    if args.stop:
        controller.stop_stream()

    if args.start:
        process = controller.start_stream(detached=not args.wait)
        if args.wait:
            if process is None:
                raise RuntimeError("stream wait requested but no attached process was started")
            rc = controller.wait()
            if process.stdout:
                output = process.stdout.read()
                if output:
                    print(output, end="")
            ok = (rc == 0) and ok

    for entry in event_log.entries:
        print(f"{entry.timestamp} [{entry.level}] {entry.message}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
