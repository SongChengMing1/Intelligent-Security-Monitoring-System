from __future__ import annotations

import json
import shlex
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from .config import ViewerConfig
from .recording import RecordingPlan
from .tracking_profile import effective_profile_mismatches, profile_hash


@dataclass
class LogEntry:
    timestamp: str
    level: str
    message: str


class EventLog:
    def __init__(self, log_path: Optional[Path] = None) -> None:
        self.entries: List[LogEntry] = []
        self.log_path = log_path
        if self.log_path:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)

    def add(self, level: str, message: str) -> None:
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        entry = LogEntry(timestamp=timestamp, level=level, message=message)
        self.entries.append(entry)
        line = f"{entry.timestamp} [{entry.level}] {entry.message}"
        if self.log_path:
            with self.log_path.open("a", encoding="utf-8") as fp:
                fp.write(line + "\n")

    def info(self, message: str) -> None:
        self.add("INFO", message)

    def error(self, message: str) -> None:
        self.add("ERROR", message)


class BoardController:
    def __init__(self, config: Optional[ViewerConfig] = None, event_log: Optional[EventLog] = None) -> None:
        self.config = config or ViewerConfig()
        self.log = event_log or EventLog()
        self.process: Optional[subprocess.Popen[str]] = None

    def _build_stream_args(self) -> List[str]:
        cfg = self.config
        resolved = cfg.tracking_profile
        box_thresh = resolved["detector"]["high_threshold"] if resolved else cfg.box_thresh
        nms_thresh = resolved["detector"]["nms_threshold"] if resolved else cfg.nms_thresh
        stream_args = [
            cfg.stream_executable,
            "--max-seconds",
            str(cfg.max_seconds),
            "--port",
            str(cfg.stream_port),
            "--rtsp-port",
            str(cfg.rtsp_port),
            "--rtsp-path",
            cfg.rtsp_path,
            "--rtsp-fps",
            str(cfg.rtsp_fps),
            "--h264-bitrate",
            str(cfg.h264_bitrate),
            "--display-mode",
            cfg.display_mode,
            "--output-delay-ms",
            str(cfg.output_delay_ms),
            "--latest-hold-ms",
            str(cfg.latest_hold_ms),
            "--frame-ring-size",
            str(cfg.frame_ring_size),
            "--video-node",
            cfg.video_node,
            "--width",
            str(cfg.width),
            "--height",
            str(cfg.height),
            "--fps",
            str(cfg.fps),
            "--inference-fps",
            str(cfg.inference_fps),
            "--stale-threshold-ms",
            str(cfg.stale_threshold_ms),
            "--pixel-format",
            cfg.pixel_format,
            "--preprocess-mode",
            cfg.preprocess_mode,
            "--box-thresh",
            f"{box_thresh:.6f}",
            "--nms-thresh",
            f"{nms_thresh:.6f}",
            cfg.model_path,
        ]
        if resolved:
            observation = resolved["observation"]
            tracker = resolved["tracker"]
            roi = observation["roi"]
            roi_arg = "disabled" if not roi["enabled"] else ",".join(
                f"{roi[key]:.6f}" for key in ("x1", "y1", "x2", "y2")
            )
            stream_args[1:1] = [
                "--profile-id", resolved["profile_id"],
                "--profile-hash", profile_hash(resolved),
                "--tracker-class-ids", ",".join(str(value) for value in observation["allowed_class_ids"]),
                "--tracker-min-width", f"{observation['min_width']:.6f}",
                "--tracker-min-height", f"{observation['min_height']:.6f}",
                "--tracker-min-area", f"{observation['min_area']:.6f}",
                "--tracker-edge-margin", f"{observation['edge_margin']:.6f}",
                "--tracker-roi", roi_arg,
                "--tracker-type", tracker["type"],
                "--tracker-low-thresh", f"{tracker['low_threshold']:.6f}",
                "--tracker-high-thresh", f"{tracker['high_threshold']:.6f}",
                "--tracker-new-track-thresh", f"{tracker['new_track_threshold']:.6f}",
                "--tracker-match-thresh", f"{tracker['match_threshold']:.6f}",
                "--tracker-second-match-thresh", f"{tracker['second_match_threshold']:.6f}",
                "--tracker-confirm-hits", str(tracker["confirm_hits"]),
                "--tracker-lost-timeout-ms", str(tracker["lost_timeout_ms"]),
                "--tracker-max-tracks", str(tracker["max_tracks"]),
            ]
        event_profile = cfg.event_profile
        if event_profile is not None:
            rule = event_profile["rule"]
            region = rule["region"]
            intrusion_args = [
                "--intrusion-schema-version",
                str(event_profile["event_profile_schema_version"]),
                "--intrusion-camera-id",
                event_profile["camera_id"],
                "--intrusion-enabled" if event_profile["enabled"] else "--intrusion-disabled",
            ]
            if event_profile["enabled"]:
                intrusion_args.extend(
                    [
                        "--intrusion-rule-id",
                        rule["rule_id"],
                        "--intrusion-class-ids",
                        ",".join(str(value) for value in rule["class_ids"]),
                        "--intrusion-region",
                        ",".join(f"{region[key]:.6f}" for key in ("x1", "y1", "x2", "y2")),
                        "--intrusion-dwell-ms",
                        str(rule["dwell_ms"]),
                        "--intrusion-boundary-hysteresis-px",
                        f"{rule['boundary_hysteresis_px']:.6f}",
                        "--intrusion-prediction-grace-ms",
                        str(rule["prediction_grace_ms"]),
                    ]
                )
            stream_args[1:1] = intrusion_args
        if cfg.recording_plan is not None:
            stream_args.extend(cfg.recording_plan.stream_args())
        return stream_args

    def validate_effective_profile(self, status: dict) -> bool:
        if self.config.tracking_profile is None:
            return True
        mismatches = effective_profile_mismatches(status, self.config.tracking_profile)
        for mismatch in mismatches:
            self.log.error(f"effective tracking Profile mismatch: {mismatch}")
        return not mismatches

    def _build_quoted_stream_command(self) -> str:
        return " ".join(shlex.quote(part) for part in self._build_stream_args())

    def build_remote_stream_command(self) -> str:
        cfg = self.config
        quoted_stream = self._build_quoted_stream_command()
        commands = [
            f"cd {shlex.quote(cfg.remote_project_dir)}",
            f"LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH {quoted_stream}",
        ]
        return " && ".join(commands)

    def build_remote_stream_detached_command(self) -> str:
        cfg = self.config
        quoted_stream = self._build_quoted_stream_command()
        pid_path = shlex.quote(cfg.stream_pid_path)
        log_path = shlex.quote(cfg.stream_log_path)
        remote_output_dir = shlex.quote(cfg.remote_output_dir)
        launch_script = (
            f"echo $$ > {pid_path}; "
            f"cd {shlex.quote(cfg.remote_project_dir)} && "
            "export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH; "
            f"exec {quoted_stream} "
            f"> {log_path} 2>&1 < /dev/null"
        )
        commands = [
            f"mkdir -p {remote_output_dir}",
            f"rm -f {pid_path}",
            f"setsid -f sh -c {shlex.quote(launch_script)} >/dev/null 2>&1",
            "sleep 0.5",
            f"pid=$(cat {pid_path} 2>/dev/null || true)",
            (
                f"if [ -n \"$pid\" ] && kill -0 \"$pid\" 2>/dev/null; then "
                f"echo stream_server_pid=$pid; "
                f"echo stream_server_log={shlex.quote(cfg.stream_log_path)}; "
                "else "
                "echo stream server failed to stay running; "
                f"tail -n 80 {log_path} 2>/dev/null || true; "
                "exit 1; "
                "fi"
            ),
        ]
        return " && ".join(commands)

    def ssh_command(self, remote_command: str) -> List[str]:
        return self.config.ssh_base_args() + [remote_command]

    def _remote_stream_process_pattern(self) -> str:
        name = Path(self.config.stream_executable).name
        if not name:
            name = self.config.stream_executable
        return f"[{name[0]}]{name[1:]}" if len(name) > 1 else name

    def stop_remote_stream_processes(self) -> None:
        pattern = self._remote_stream_process_pattern()
        pid_path = shlex.quote(self.config.stream_pid_path)
        command = (
            f"if [ -s {pid_path} ]; then "
            f"pid=$(cat {pid_path} 2>/dev/null || true); "
            "if [ -n \"$pid\" ]; then kill -INT \"$pid\" 2>/dev/null || true; fi; "
            "fi; "
            f"for pid in $(pgrep -f {shlex.quote(pattern)} 2>/dev/null); "
            "do kill -INT \"$pid\" 2>/dev/null || true; done; "
            f"sleep 1; rm -f {pid_path}; true"
        )
        self.log.info("requesting clean stop for existing remote stream processes")
        result = subprocess.run(
            self.ssh_command(command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.stderr.strip():
            self.log.error(f"remote stream stop stderr: {result.stderr.strip()}")

    def check_ssh(self) -> bool:
        self.log.info(f"checking SSH connectivity to {self.config.ssh_target}")
        result = subprocess.run(
            self.config.ssh_base_args() + ["true"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode == 0:
            self.log.info("SSH connectivity OK")
            return True
        self.log.error(f"SSH connectivity failed: {result.stderr.strip()}")
        return False

    def _remote_recording_root(self, plan) -> str:
        root = plan.recording_root
        if not Path(root).is_absolute():
            root = f"{self.config.remote_project_dir}/{root}"
        return root

    def check_recording_preflight(self) -> bool:
        """Verify the board can create this recording Session before launch."""

        plan = self.config.recording_plan
        if plan is None or not plan.enabled:
            return True
        root = self._remote_recording_root(plan)
        session_path = f"{root}/{plan.recording_session_id}"
        probe_path = f"{root}/.host-viewer-preflight-{plan.runtime_session_id}"
        command = (
            "set -eu; "
            f"mkdir -p {shlex.quote(root)}; "
            f"test -d {shlex.quote(root)} && test -w {shlex.quote(root)}; "
            f"if [ -e {shlex.quote(session_path)} ]; then "
            "echo recording Session already exists >&2; exit 1; fi; "
            f": > {shlex.quote(probe_path)}; rm -f {shlex.quote(probe_path)}"
        )
        self.log.info(f"checking recording preflight on {self.config.ssh_target}: {root}")
        result = subprocess.run(
            self.ssh_command(command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode == 0:
            self.log.info("recording preflight OK")
            return True
        detail = result.stderr.strip() or result.stdout.strip() or f"exit code {result.returncode}"
        self.log.error(f"recording preflight failed: {detail}")
        return False

    def start_stream(self, detached: bool = True) -> Optional[subprocess.Popen[str]]:
        if self.process and self.process.poll() is None:
            raise RuntimeError("board process is already running")

        self.stop_remote_stream_processes()
        remote_command = self.build_remote_stream_detached_command() if detached else self.build_remote_stream_command()
        mode = "detached" if detached else "attached"
        self.log.info(f"starting board stream command ({mode}) on {self.config.ssh_target}: {remote_command}")
        if detached:
            result = subprocess.run(
                self.ssh_command(remote_command),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            output = result.stdout.strip()
            if output:
                for line in output.splitlines():
                    self.log.info(f"remote stream start: {line}")
            if result.stderr.strip():
                self.log.error(f"remote stream start stderr: {result.stderr.strip()}")
            if result.returncode != 0:
                raise RuntimeError(f"remote stream start failed with code {result.returncode}")
            self.process = None
            return None
        self.process = subprocess.Popen(
            self.ssh_command(remote_command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        return self.process

    def wait(self, timeout: Optional[float] = None) -> int:
        if not self.process:
            raise RuntimeError("board process has not been started")
        try:
            rc = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.log.error("board process wait timed out")
            raise
        self.log.info(f"board process exited with code {rc}")
        return rc

    def is_local_session_running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def is_remote_stream_running(self) -> bool:
        pattern = self._remote_stream_process_pattern()
        pid_path = shlex.quote(self.config.stream_pid_path)
        command = (
            f"pid=$(cat {pid_path} 2>/dev/null || true); "
            "if [ -n \"$pid\" ] && kill -0 \"$pid\" 2>/dev/null; then "
            "echo stream_server_pid=$pid; exit 0; "
            "fi; "
            f"pid=$(pgrep -f {shlex.quote(pattern)} 2>/dev/null | head -n 1); "
            "if [ -n \"$pid\" ]; then echo stream_server_pid=$pid; exit 0; fi; "
            "exit 1"
        )
        result = subprocess.run(
            self.ssh_command(command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        running = result.returncode == 0
        detail = result.stdout.strip()
        self.log.info(f"remote stream running: {running}" + (f" {detail}" if detail else ""))
        if result.stderr.strip():
            self.log.error(f"remote stream status stderr: {result.stderr.strip()}")
        return running

    def stop_stream(self) -> None:
        if self.process and self.process.poll() is None:
            self.log.info("terminating local SSH stream session")
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.log.error("local SSH stream session did not exit after terminate; killing it")
                self.process.kill()
                self.process.wait(timeout=3)

        self.stop_remote_stream_processes()

    def fetch_recording_status(self) -> dict:
        plan = self.config.recording_plan
        if plan is None or not plan.enabled:
            return {"state": "disabled"}
        root = self._remote_recording_root(plan)
        status_path = f"{root}/{plan.recording_session_id}/session-status.json"
        command = f"cat {shlex.quote(status_path)}"
        result = subprocess.run(
            self.ssh_command(command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0 or not result.stdout.strip():
            return {}
        try:
            value = json.loads(result.stdout)
        except json.JSONDecodeError:
            return {}
        return value if isinstance(value, dict) else {}

    def wait_for_recording_terminal(self, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + max(0.0, timeout)
        last_status: dict = {}
        terminal = {"finalized", "limit_reached", "failed", "interrupted"}
        while time.monotonic() <= deadline:
            last_status = self.fetch_recording_status()
            if last_status.get("state") in terminal:
                self.log.info(f"recording terminal state: {last_status.get('state')}")
                return last_status
            time.sleep(0.25)
        self.log.error("recording terminal state was not observed before timeout")
        return last_status

    @staticmethod
    def _is_complete_recording_session(session_path: Path) -> bool:
        return (
            session_path.is_dir()
            and (session_path / "manifest.json").is_file()
            and (session_path / "session-status.json").is_file()
            and any(session_path.glob("records-*.jsonl"))
        )

    def copy_recording_session(self, plan: RecordingPlan, destination_root: Path) -> Path:
        """Copy one complete remote recording Session into the host project.

        The copy is staged below the destination and published only after the
        expected Session files are present.  This prevents Replay from seeing
        a partially copied directory when it is opened immediately after Stop.
        """

        if not plan.enabled:
            raise ValueError("cannot copy a disabled recording plan")
        if not plan.recording_session_id:
            raise ValueError("recording plan has no recording session ID")

        destination_root = Path(destination_root)
        destination_root.mkdir(parents=True, exist_ok=True)
        destination = destination_root / plan.recording_session_id
        if destination.exists():
            if self._is_complete_recording_session(destination):
                self.log.info(f"recording Session already available locally: {destination}")
                return destination
            raise RuntimeError(f"local recording Session directory is incomplete: {destination}")

        staging_root = Path(tempfile.mkdtemp(prefix=f".{plan.recording_session_id}.", dir=str(destination_root)))
        remote_session = f"{self._remote_recording_root(plan)}/{plan.recording_session_id}"
        source = f"{self.config.ssh_target}:{remote_session}"
        self.log.info(f"copying complete recording Session {source} -> {destination}")
        try:
            result = subprocess.run(
                self.config.scp_base_args() + ["-r", source, str(staging_root)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if result.returncode != 0:
                detail = result.stderr.strip() or result.stdout.strip() or f"exit code {result.returncode}"
                raise RuntimeError(f"recording Session copy failed: {detail}")

            staged_session = staging_root / plan.recording_session_id
            if not self._is_complete_recording_session(staged_session):
                raise RuntimeError(
                    "recording Session copy failed: copied directory is missing "
                    "manifest.json, session-status.json or records-*.jsonl"
                )
            if destination.exists():
                raise RuntimeError(f"local recording Session directory appeared during copy: {destination}")
            staged_session.rename(destination)
            self.log.info(f"recording Session ready for Replay: {destination}")
            return destination
        finally:
            shutil.rmtree(staging_root, ignore_errors=True)
