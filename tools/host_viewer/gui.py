from __future__ import annotations

import argparse
import json
import sys
import tempfile
import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from PySide6.QtCore import Qt, QThread, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen, QPixmap, QTransform
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .config import ViewerConfig
from .controller import BoardController, EventLog
from .event_profile import EventProfileError, load_event_profile, resolve_event_profile
from .profile_catalog import (
    CameraRegion,
    CameraRegionError,
    ProfileCatalogError,
    TrackingProfilePreset,
    apply_camera_region,
    discover_tracking_profiles,
    load_camera_region,
)
from .recording import RecorderState, RecordingPlan, RecordingWorkflow
from .replay import (
    REPLAY_LAYERS,
    REPLAY_MODES,
    REPLAY_RERUN_LAYERS,
    ReplayController,
    ReplayError,
    ReplayWorkspace,
    image_path,
    load_replay_session,
    observations_for_layer,
    run_replay_command,
    validate_replay_profile,
)
from .stream import (
    RtspStreamWorker,
    StreamFrame,
    StreamSnapshot,
    fetch_stream_snapshot,
    rtsp_url,
)
from .tracking_overlay import OverlaySelection, select_overlay
from .intrusion_overlay import (
    IntrusionOverlaySelection,
    advance_event_cursor,
    intrusion_has_alarm,
    intrusion_target_label,
    select_intrusion_overlay,
)
from .tracking_profile import profile_hash
from .workflow import LiveError, LiveEvidence, LiveState, LiveWorkflow


STREAM_POLL_INTERVAL_MS = 250
REMOTE_PROBE_INTERVAL_SECONDS = 1.0
RECORDING_DRAIN_TIMEOUT_SECONDS = 5.0


class LiveOperationError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class HostViewerWindow(QMainWindow):
    """Minimal operator surface for the explicit Live and Replay workflows."""

    def __init__(self, config: Optional[ViewerConfig] = None) -> None:
        super().__init__()
        self.setWindowTitle("RK3568 Smart Monitor Host Viewer")
        self.resize(1180, 760)

        self.config = config or ViewerConfig()
        self.event_log = EventLog()
        self.controller = BoardController(self.config, self.event_log)
        self.executor = ThreadPoolExecutor(max_workers=4)
        self.backend_lock = threading.Lock()

        self.workflow = LiveWorkflow()
        self.recording_plan = RecordingPlan.create("off")
        self.recording_workflow = RecordingWorkflow(self.recording_plan)
        self.replay_workspace = ReplayWorkspace()
        self.replay_controller: Optional[ReplayController] = None

        self.live_start_future: Optional[Future[Any]] = None
        self.live_start_token: Optional[int] = None
        self.live_stop_future: Optional[Future[Any]] = None
        self.replay_load_future: Optional[Future[Any]] = None
        self.remote_probe_future: Optional[Future[Any]] = None
        self.remote_probe_time = 0.0
        self.emergency_stop_scheduled = False
        self.stream_mode_active = False
        self.stream_start_time = 0.0
        self.stream_error = ""
        self.status_available = False
        self.latest_stream_status: Dict[str, Any] = {}
        self.latest_detections: Dict[str, Any] = {}
        self.latest_tracks: Dict[str, Any] = {}
        self.latest_events: Dict[str, Any] = {}
        self.latest_event_errors: list[str] = []
        self.intrusion_runtime_session_id = ""
        self.last_event_sequence = 0
        self.latest_overlay = OverlaySelection()
        self.latest_intrusion_overlay = IntrusionOverlaySelection()
        self.latest_stream_frame: Optional[StreamFrame] = None
        self.stream_poll_future: Optional[Future[StreamSnapshot]] = None
        self.stream_thread: Optional[QThread] = None
        self.stream_worker: Optional[Any] = None
        self.replay_loading = False
        self.stop_cleanup_only = False
        self._last_error_logged = ""

        self.repo_root = Path(__file__).resolve().parents[2]
        self.presets: list[TrackingProfilePreset] = []
        self.profile_catalog_error = ""
        self.camera_region: Optional[CameraRegion] = None
        self.camera_region_error = ""
        self.event_profile: Optional[Dict[str, Any]] = None
        self.event_profile_error = ""

        self._build_widgets()
        self._load_profile_catalog()
        self._build_layout()
        self._connect_signals()
        self._update_profile_preview()

        self.timer = QTimer(self)
        self.timer.setInterval(STREAM_POLL_INTERVAL_MS)
        self.timer.timeout.connect(self._on_timer)
        self.timer.start()
        self._update_controls()

    def _build_widgets(self) -> None:
        self.mode_combo = QComboBox()
        self.mode_combo.addItems(["Live", "Replay"])

        self.profile_combo = QComboBox()
        self.profile_preview_label = QLabel("-")
        self.profile_preview_label.setWordWrap(True)
        self.camera_combo = QComboBox()
        self.camera_combo.addItem(self.config.camera_id, self.config.camera_id)
        self.camera_region_label = QLabel("-")
        self.camera_region_label.setWordWrap(True)
        self.roi_enabled_check = QCheckBox("Enable normalized rectangle ROI")
        self.roi_spins: Dict[str, QDoubleSpinBox] = {}
        for key, value in (("x1", 0.0), ("y1", 0.0), ("x2", 1.0), ("y2", 1.0)):
            spin = QDoubleSpinBox()
            spin.setRange(0.0, 1.0)
            spin.setDecimals(3)
            spin.setSingleStep(0.01)
            spin.setValue(value)
            self.roi_spins[key] = spin

        self.recording_combo = QComboBox()
        for label, mode in (
            ("Off", "off"),
            ("Metadata-only", "metadata"),
            ("Sampled JPEG", "sampled"),
            ("All JPEG", "all"),
        ):
            self.recording_combo.addItem(label, mode)

        self.rotate_spin = QDoubleSpinBox()
        self.rotate_spin.setRange(-270.0, 270.0)
        self.rotate_spin.setDecimals(0)
        self.rotate_spin.setSingleStep(90.0)
        self.rotate_spin.setSuffix(" deg")
        self.rotate_spin.setValue(0.0)

        self.replay_session_edit = QLineEdit()
        self.replay_browse_button = QPushButton("Browse")
        self.load_replay_button = QPushButton("Load Replay")
        self.replay_mode_combo = QComboBox()
        self.replay_mode_combo.addItems(list(REPLAY_MODES))
        self.replay_layer_combo = QComboBox()
        self._set_replay_layers(REPLAY_LAYERS, "recorded-tracks")
        self.replay_previous_button = QPushButton("Previous")
        self.replay_next_button = QPushButton("Next")
        self.replay_play_button = QPushButton("Play")

        self.start_button = QPushButton("Start")
        self.stop_button = QPushButton("Stop")

        self.image_label = QLabel("Waiting for frame")
        self.image_label.setAlignment(Qt.AlignCenter)
        self.image_label.setMinimumSize(720, 540)
        self.image_label.setStyleSheet("background:#111; color:#ddd; border:1px solid #333;")

        self.state_label = QLabel(LiveState.IDLE.value)
        self.recording_state_label = QLabel(RecorderState.DISABLED.value)
        self.runtime_session_label = QLabel("-")
        self.recording_session_label = QLabel("-")
        self.recording_local_path_label = QLabel("-")
        self.profile_label = QLabel("-")
        self.roi_label = QLabel("disabled")
        self.board_status_label = QLabel("not started")
        self.preview_fps_label = QLabel("-")
        self.inference_fps_label = QLabel("-")
        self.tracker_label = QLabel("-")
        self.tracks_label = QLabel("-")
        self.intrusion_label = QLabel("unavailable")
        self.overlay_source_label = QLabel("-")
        self.metadata_age_label = QLabel("-")
        self.detections_label = QLabel("-")
        self.stale_label = QLabel("not refreshed")
        for label in (
            self.recording_local_path_label,
            self.tracker_label,
            self.tracks_label,
            self.overlay_source_label,
        ):
            label.setWordWrap(True)
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)

    def _profile_directory(self) -> Path:
        path = Path(self.config.tracking_profiles_dir)
        return path if path.is_absolute() else self.repo_root / path

    def _camera_directory(self) -> Path:
        path = Path(self.config.camera_regions_dir)
        return path if path.is_absolute() else self.repo_root / path

    def _local_recording_root(self) -> Path:
        return self.repo_root / "recordings"

    def _load_profile_catalog(self) -> None:
        try:
            self.presets = discover_tracking_profiles(self._profile_directory())
            for preset in self.presets:
                self.profile_combo.addItem(f"{preset.profile_id} — {preset.description}", preset.profile_id)
            default_index = self.profile_combo.findData("default-general")
            if default_index >= 0:
                self.profile_combo.setCurrentIndex(default_index)
        except ProfileCatalogError as exc:
            self.profile_catalog_error = str(exc)
            self.profile_combo.addItem("Invalid Profile catalog", "")
            self.event_log.error(self.profile_catalog_error)

        camera_path = self._camera_directory() / f"{self.config.camera_id}.json"
        try:
            self.camera_region = load_camera_region(camera_path)
            self.roi_enabled_check.setChecked(bool(self.camera_region.roi["enabled"]))
            for key, spin in self.roi_spins.items():
                spin.setValue(float(self.camera_region.roi[key]))
        except CameraRegionError as exc:
            self.camera_region_error = str(exc)
            self.event_log.error(self.camera_region_error)

        try:
            if self.config.event_profile is not None:
                self.event_profile = resolve_event_profile(self.config.event_profile, self.config.camera_id)
            elif self.config.event_profile_path is not None:
                path = Path(self.config.event_profile_path)
                if not path.is_absolute():
                    path = self.repo_root / path
                self.event_profile = load_event_profile(path, self.config.camera_id)
        except EventProfileError as exc:
            self.event_profile_error = str(exc)
            self.event_log.error(self.event_profile_error)

    def _build_layout(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        main_layout = QHBoxLayout(root)

        left_layout = QVBoxLayout()
        left_layout.addWidget(self.image_label, stretch=1)
        action_row = QHBoxLayout()
        action_row.addWidget(self.start_button)
        action_row.addWidget(self.stop_button)
        left_layout.addLayout(action_row)
        main_layout.addLayout(left_layout, stretch=3)

        right_layout = QVBoxLayout()
        workspace_box = QGroupBox("Workspace")
        workspace_form = QFormLayout(workspace_box)
        workspace_form.addRow("Mode", self.mode_combo)
        workspace_form.addRow("Rotate", self.rotate_spin)
        right_layout.addWidget(workspace_box)

        live_box = QGroupBox("Live setup")
        live_form = QFormLayout(live_box)
        live_form.addRow("Scene", self.profile_combo)
        live_form.addRow("Scene preview", self.profile_preview_label)
        live_form.addRow("Camera", self.camera_combo)
        live_form.addRow("Camera region", self.camera_region_label)
        live_form.addRow(self.roi_enabled_check)
        roi_row = QGridLayout()
        for column, key in enumerate(("x1", "y1", "x2", "y2")):
            roi_row.addWidget(QLabel(key), 0, column)
            roi_row.addWidget(self.roi_spins[key], 1, column)
        live_form.addRow(roi_row)
        live_form.addRow("Recording", self.recording_combo)
        right_layout.addWidget(live_box)

        replay_box = QGroupBox("Replay")
        replay_form = QFormLayout(replay_box)
        session_row = QHBoxLayout()
        session_row.addWidget(self.replay_session_edit)
        session_row.addWidget(self.replay_browse_button)
        replay_form.addRow("Local Session", session_row)
        replay_form.addRow("Replay mode", self.replay_mode_combo)
        replay_form.addRow("Layer", self.replay_layer_combo)
        replay_actions = QHBoxLayout()
        replay_actions.addWidget(self.load_replay_button)
        replay_actions.addWidget(self.replay_previous_button)
        replay_actions.addWidget(self.replay_next_button)
        replay_actions.addWidget(self.replay_play_button)
        replay_form.addRow(replay_actions)
        right_layout.addWidget(replay_box)

        metrics_box = QGroupBox("Status")
        metrics_grid = QGridLayout(metrics_box)
        rows = [
            ("State", self.state_label),
            ("Recorder", self.recording_state_label),
            ("Runtime ID", self.runtime_session_label),
            ("Recording ID", self.recording_session_label),
            ("Local Session", self.recording_local_path_label),
            ("Profile", self.profile_label),
            ("ROI", self.roi_label),
            ("Board", self.board_status_label),
            ("RTSP FPS", self.preview_fps_label),
            ("Inference FPS", self.inference_fps_label),
            ("Tracker", self.tracker_label),
            ("Tracks", self.tracks_label),
            ("Intrusion", self.intrusion_label),
            ("Overlay", self.overlay_source_label),
            ("Metadata age", self.metadata_age_label),
            ("Raw detections", self.detections_label),
        ]
        for row, (name, widget) in enumerate(rows):
            metrics_grid.addWidget(QLabel(name), row, 0)
            metrics_grid.addWidget(widget, row, 1)
        right_layout.addWidget(metrics_box)
        right_layout.addWidget(self.stale_label)
        right_layout.addWidget(self.log_text, stretch=1)
        main_layout.addLayout(right_layout, stretch=1)

    def _connect_signals(self) -> None:
        self.mode_combo.currentTextChanged.connect(self._on_mode_changed)
        self.profile_combo.currentIndexChanged.connect(self._update_profile_preview)
        self.camera_combo.currentIndexChanged.connect(self._update_profile_preview)
        self.roi_enabled_check.toggled.connect(self._on_roi_toggled)
        for spin in self.roi_spins.values():
            spin.valueChanged.connect(self._update_profile_preview)
        self.recording_combo.currentIndexChanged.connect(self._update_controls)
        self.start_button.clicked.connect(self._start)
        self.stop_button.clicked.connect(self._stop)
        self.replay_browse_button.clicked.connect(self._browse_replay)
        self.load_replay_button.clicked.connect(self._load_replay)
        self.replay_previous_button.clicked.connect(lambda: self._step_replay(-1))
        self.replay_next_button.clicked.connect(lambda: self._step_replay(1))
        self.replay_play_button.clicked.connect(self._toggle_replay)
        self.replay_mode_combo.currentTextChanged.connect(self._on_replay_mode_changed)
        self.replay_layer_combo.currentTextChanged.connect(self._apply_replay_record)

    def closeEvent(self, event: Any) -> None:
        start_pending = self.live_start_future is not None and not self.live_start_future.done()
        stop_pending = self.live_stop_future is not None and not self.live_stop_future.done()
        live_active = self.stream_mode_active or start_pending or stop_pending or self.workflow.state in {
            LiveState.STARTING,
            LiveState.RUNNING,
            LiveState.DEGRADED,
            LiveState.STOPPING,
        }
        if live_active:
            if self._stop_confirmation_required() and not self._confirm_recording_stop():
                event.ignore()
                return
            if self.recording_plan.enabled:
                self.recording_workflow.request_stop(confirmed=True)
            self.workflow.request_stop()
            self._stop_stream_worker()
            stop_error = ""
            status: Dict[str, Any] = {}
            try:
                status = self._perform_live_stop(self.recording_plan)
            except Exception as exc:
                stop_error = str(exc)
                self.event_log.error(f"Live close cleanup failed: {exc}")
            if self.recording_plan.enabled:
                self._apply_recording_stop_status(status, context="close")
            self.stream_mode_active = False
            self.workflow.complete_stop(stop_error or None)
        self.replay_workspace.stop()
        self.executor.shutdown(wait=False)
        event.accept()

    def _on_mode_changed(self, mode: str) -> None:
        if mode == "Live" and self.replay_workspace.controller is not None:
            self.mode_combo.blockSignals(True)
            self.mode_combo.setCurrentText("Replay")
            self.mode_combo.blockSignals(False)
            self.event_log.error("Stop Replay before switching to Live")
            return
        self._update_controls()

    def _on_roi_toggled(self, enabled: bool) -> None:
        for spin in self.roi_spins.values():
            spin.setEnabled(enabled and self._live_settings_unlocked())
        self._update_profile_preview()

    def _on_replay_mode_changed(self, mode: str) -> None:
        if self.replay_workspace.controller is None:
            self._set_replay_layers(REPLAY_RERUN_LAYERS if mode != "recorded" else REPLAY_LAYERS,
                                    "replayed-tracks" if mode != "recorded" else "recorded-tracks")

    def _set_replay_layers(self, layers: Tuple[str, ...], default: str) -> None:
        self.replay_layer_combo.blockSignals(True)
        self.replay_layer_combo.clear()
        self.replay_layer_combo.addItems(list(layers))
        index = self.replay_layer_combo.findText(default)
        if index >= 0:
            self.replay_layer_combo.setCurrentIndex(index)
        self.replay_layer_combo.blockSignals(False)

    def _live_settings_unlocked(self) -> bool:
        return self._is_live_mode() and self.workflow.state in {LiveState.IDLE, LiveState.ERROR}

    def _is_live_mode(self) -> bool:
        return self.mode_combo.currentText() == "Live"

    def _is_replay_mode(self) -> bool:
        return self.mode_combo.currentText() == "Replay"

    def _selected_preset(self) -> TrackingProfilePreset:
        profile_id = self.profile_combo.currentData()
        for preset in self.presets:
            if preset.profile_id == profile_id:
                return preset
        raise ProfileCatalogError(self.profile_catalog_error or "no valid Tracking Profile is selected")

    def _selected_camera_region(self) -> CameraRegion:
        if self.camera_region is None:
            raise CameraRegionError(self.camera_region_error or "camera region is unavailable")
        roi = {"enabled": self.roi_enabled_check.isChecked()}
        roi.update({key: spin.value() for key, spin in self.roi_spins.items()})
        return CameraRegion(
            camera_id=self.camera_region.camera_id,
            description=self.camera_region.description,
            roi=roi,
            source_path=self.camera_region.source_path,
        )

    def _effective_profile(self) -> Dict[str, Any]:
        return apply_camera_region(self._selected_preset().resolved, self._selected_camera_region())

    def _roi_summary(self, profile: Dict[str, Any]) -> str:
        roi = profile["observation"]["roi"]
        if not roi["enabled"]:
            return "disabled"
        return "({x1:.3f}, {y1:.3f})–({x2:.3f}, {y2:.3f})".format(**roi)

    def _update_profile_preview(self, *_args: Any) -> None:
        try:
            preset = self._selected_preset()
            profile = self._effective_profile()
            self.profile_preview_label.setText(preset.description)
            self.profile_label.setText(f"{profile['profile_id']} {profile_hash(profile)}")
            self.roi_label.setText(self._roi_summary(profile))
        except (ProfileCatalogError, CameraRegionError, ValueError) as exc:
            self.profile_preview_label.setText(f"Invalid setup: {exc}")
            self.profile_label.setText("invalid")
            self.roi_label.setText("invalid")

    def _prepare_live_config(self) -> None:
        if self.event_profile_error:
            raise EventProfileError(self.event_profile_error)
        profile = self._effective_profile()
        mode = str(self.recording_combo.currentData() or "off")
        plan = RecordingPlan.create(mode)
        self.config = self.config.with_overrides(tracking_profile=profile, recording_plan=plan)
        self.config.event_profile = self.event_profile
        self.controller.config = self.config
        self.recording_plan = plan
        self.recording_workflow = RecordingWorkflow(plan)
        self.runtime_session_label.setText(plan.runtime_session_id)
        self.recording_session_label.setText(plan.recording_session_id or "-")
        self.recording_local_path_label.setText(
            str(self._local_recording_root() / plan.recording_session_id) if plan.enabled else "-"
        )
        self.profile_label.setText(f"{profile['profile_id']} {profile_hash(profile)}")
        self.roi_label.setText(self._roi_summary(profile))

    def _start(self) -> None:
        if not self._is_live_mode():
            return
        if self.workflow.state not in {LiveState.IDLE, LiveState.ERROR}:
            return
        try:
            self._prepare_live_config()
            token = self.workflow.begin_start()
        except Exception as exc:
            self.event_log.error(f"Start preflight rejected: {exc}")
            self.workflow.state = LiveState.ERROR
            self.workflow.error = LiveError("preflight", str(exc))
            self._set_state_label()
            self._append_logs()
            return

        self.emergency_stop_scheduled = False
        self._reset_stream_data()
        self.stream_start_time = time.monotonic()
        self.live_start_token = token
        self.live_start_future = self.executor.submit(self._perform_live_start)
        self._set_state_label()
        self._append_logs()

    def _perform_live_start(self) -> bool:
        with self.backend_lock:
            if not self.controller.check_ssh():
                raise LiveOperationError("preflight", f"SSH preflight failed for {self.config.ssh_target}")
            if not self.controller.check_recording_preflight():
                raise LiveOperationError("preflight", "recording Session directory is not writable or already exists")
            self.controller.start_stream(detached=True)
        return True

    def _stop_confirmation_required(self) -> bool:
        return self.recording_plan.enabled and not self.recording_workflow.live_can_release

    def _confirm_recording_stop(self) -> bool:
        answer = QMessageBox.question(
            self,
            "Finalize recording",
            "Recording is enabled. Stop will drain pending records before releasing Live. Continue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.Yes,
        )
        return answer == QMessageBox.StandardButton.Yes

    def _stop(self) -> None:
        if self._is_replay_mode():
            if self.replay_workspace.controller is not None:
                self.replay_workspace.stop()
                self.replay_controller = None
                self.image_label.setText("Waiting for frame")
                self.state_label.setText("Replay Empty")
                self._update_controls()
            return
        if self.workflow.state == LiveState.IDLE:
            return
        if self._stop_confirmation_required() and not self._confirm_recording_stop():
            return
        if self.recording_plan.enabled:
            self.recording_workflow.request_stop(confirmed=True)
        self._request_live_stop()

    def _request_live_stop(self, *, cleanup_only: bool = False) -> None:
        if self.live_stop_future is not None and not self.live_stop_future.done():
            return
        self.stop_cleanup_only = cleanup_only
        if not cleanup_only:
            self.workflow.request_stop()
        self._stop_stream_worker()
        plan = self.recording_plan
        self.live_stop_future = self.executor.submit(self._perform_live_stop, plan)
        self._set_state_label()

    def _perform_live_stop(self, plan: RecordingPlan) -> Dict[str, Any]:
        with self.backend_lock:
            self.controller.stop_stream()
            if plan.enabled:
                status = dict(self.controller.wait_for_recording_terminal(RECORDING_DRAIN_TIMEOUT_SECONDS))
                if status.get("state") in {
                    RecorderState.FINALIZED.value,
                    RecorderState.LIMIT_REACHED.value,
                }:
                    try:
                        local_path = self.controller.copy_recording_session(plan, self._local_recording_root())
                    except Exception as exc:
                        status["recording_copy_error"] = str(exc)
                    else:
                        status["local_session_path"] = str(local_path)
                return status
        return {"state": "disabled"}

    def _apply_recording_stop_status(self, status: Dict[str, Any], *, context: str) -> None:
        if status:
            self.recording_workflow.observe(status)
            local_session_path = status.get("local_session_path")
            if isinstance(local_session_path, str) and local_session_path:
                self.recording_local_path_label.setText(local_session_path)
                self.replay_session_edit.setText(local_session_path)
                self.event_log.info(f"local recording Session ready: {local_session_path}")
            else:
                copy_error = status.get("recording_copy_error")
                if isinstance(copy_error, str) and copy_error:
                    self.recording_local_path_label.setText(f"copy failed: {copy_error}")
                    self.event_log.error(f"recording Session copy failed: {copy_error}")
                else:
                    state = status.get("state", "unknown")
                    self.recording_local_path_label.setText(f"not copied ({state})")
        elif self.recording_plan.enabled:
            self.recording_local_path_label.setText("not copied (unknown)")
        if not self.recording_workflow.live_can_release:
            self.recording_workflow.mark_interrupted(
                f"recording terminal state unavailable {context} Stop"
            )

    def _reset_stream_data(self) -> None:
        self.stream_mode_active = False
        self.stream_error = ""
        self.status_available = False
        self.latest_stream_status = {}
        self.latest_detections = {}
        self.latest_tracks = {}
        self.latest_events = {}
        self.latest_event_errors = []
        self.intrusion_runtime_session_id = ""
        self.last_event_sequence = 0
        self.latest_overlay = OverlaySelection()
        self.latest_intrusion_overlay = IntrusionOverlaySelection()
        self.latest_stream_frame = None
        self.board_status_label.setText("starting")
        self.intrusion_label.setText("unavailable")
        self.stale_label.setText("RTSP starting")

    def _start_rtsp_worker(self) -> None:
        self._stop_stream_worker()
        url = rtsp_url(self.config)
        self.event_log.info(f"connecting RTSP stream {url}")
        self.stream_thread = QThread(self)
        self.stream_worker = RtspStreamWorker(url)
        self.stream_worker.moveToThread(self.stream_thread)
        self.stream_thread.started.connect(self.stream_worker.run)
        self.stream_worker.frame_ready.connect(self._apply_stream_frame)
        self.stream_worker.error.connect(self._on_stream_error)
        self.stream_worker.stopped.connect(self.stream_thread.quit)
        self.stream_worker.stopped.connect(self.stream_worker.deleteLater)
        self.stream_thread.finished.connect(self.stream_thread.deleteLater)
        self.stream_thread.finished.connect(self._on_stream_thread_finished)
        self.stream_thread.start()

    def _stop_stream_worker(self) -> None:
        if self.stream_worker:
            self.stream_worker.stop()
        if self.stream_thread and self.stream_thread.isRunning():
            self.stream_thread.quit()
            self.stream_thread.wait(1500)
        self.stream_worker = None
        self.stream_thread = None

    def _on_stream_thread_finished(self) -> None:
        self.stream_worker = None
        self.stream_thread = None

    def _on_stream_error(self, message: str) -> None:
        self.stream_error = message
        if message != self._last_error_logged:
            self.event_log.error(message)
            self._last_error_logged = message
        self.stale_label.setText("RTSP degraded")
        self._observe_live()

    def _schedule_stream_poll(self) -> None:
        if self.stream_poll_future and not self.stream_poll_future.done():
            return
        self.stream_poll_future = self.executor.submit(fetch_stream_snapshot, self.config)

    def _schedule_remote_probe(self) -> None:
        if not self.stream_mode_active or self.workflow.state in {LiveState.STOPPING, LiveState.ERROR}:
            return
        now = time.monotonic()
        if self.remote_probe_future and not self.remote_probe_future.done():
            return
        if now - self.remote_probe_time < REMOTE_PROBE_INTERVAL_SECONDS:
            return
        self.remote_probe_time = now
        self.remote_probe_future = self.executor.submit(self.controller.is_remote_stream_running)

    def _on_timer(self) -> None:
        self._handle_live_futures()
        self._handle_replay_load()
        self._handle_remote_probe()

        if self._is_replay_mode():
            self._advance_replay()
        elif self._is_live_mode() and self.stream_mode_active and self.workflow.state not in {LiveState.STOPPING, LiveState.ERROR}:
            self._handle_stream_poll()
            self._schedule_remote_probe()
        self._append_logs()

    def _handle_live_futures(self) -> None:
        if self.live_start_future and self.live_start_future.done():
            future = self.live_start_future
            self.live_start_future = None
            try:
                future.result()
            except Exception as exc:
                if self.workflow.state == LiveState.STARTING and self.live_start_token is not None:
                    code = exc.code if isinstance(exc, LiveOperationError) else "launch"
                    self.workflow.complete_start(self.live_start_token, str(exc), code=code)
                    self.event_log.error(f"Live start failed ({code}): {exc}")
                elif self.workflow.state == LiveState.STOPPING:
                    self.event_log.error(f"cancelled Live start ended with: {exc}")
            else:
                if self.workflow.state == LiveState.STARTING and self.live_start_token is not None:
                    self.workflow.complete_start(self.live_start_token)
                    self.stream_mode_active = True
                    self._start_rtsp_worker()
                    self._schedule_stream_poll()
                    self.stale_label.setText("waiting for aligned evidence")
            self.live_start_token = None
            self._set_state_label()

        if self.live_stop_future and self.live_stop_future.done():
            future = self.live_stop_future
            self.live_stop_future = None
            try:
                status = future.result()
            except Exception as exc:
                status = {}
                self.event_log.error(f"Live stop failed: {exc}")
                stop_error = str(exc)
            else:
                stop_error = ""
            if self.recording_plan.enabled:
                self._apply_recording_stop_status(status, context="after")
            self.stream_mode_active = False
            if self.stop_cleanup_only:
                self.stop_cleanup_only = False
                if stop_error:
                    self.event_log.error(stop_error)
            elif self.workflow.state == LiveState.STOPPING:
                self.workflow.complete_stop(stop_error or None)
            self._set_state_label()

    def _handle_remote_probe(self) -> None:
        if not self.remote_probe_future or not self.remote_probe_future.done():
            return
        future = self.remote_probe_future
        self.remote_probe_future = None
        try:
            running = bool(future.result())
        except Exception as exc:
            self.event_log.error(f"remote stream probe failed: {exc}")
            return
        if not running and self.stream_mode_active and self.workflow.state not in {LiveState.STOPPING, LiveState.ERROR}:
            if time.monotonic() - self.stream_start_time >= 5.0:
                self.workflow.observe(LiveEvidence(process_alive=False, fatal_error="board stream process exited"))
                self.stream_mode_active = False
                self._stop_stream_worker()
                self.event_log.error("board stream process exited; automatic restart is disabled")
                self._set_state_label()

    def _handle_stream_poll(self) -> None:
        if self.stream_poll_future and self.stream_poll_future.done():
            future = self.stream_poll_future
            self.stream_poll_future = None
            try:
                self._apply_stream_snapshot(future.result())
            except Exception as exc:
                self.event_log.error(f"stream poll failed: {exc}")
        if self.stream_poll_future is None:
            self._schedule_stream_poll()

    def _recording_status_from_stream(self, status: Dict[str, Any]) -> Dict[str, Any]:
        return {
            "state": status.get("recording_state", "disabled"),
            "session_id": status.get("recording_session_id", ""),
            "error": status.get("recording_error", ""),
        }

    def _apply_stream_snapshot(self, snapshot: StreamSnapshot) -> None:
        if snapshot.status:
            self.status_available = True
            self.latest_stream_status = snapshot.status
            self.board_status_label.setText(str(snapshot.status.get("state", "readable")))
            self.preview_fps_label.setText(_fmt(snapshot.status.get("rtsp_fps")))
            self.inference_fps_label.setText(_fmt(snapshot.status.get("inference_fps")))
            tracker_state = str(snapshot.status.get("tracker_state", "-"))
            tracker_error = str(snapshot.status.get("tracker_error", ""))
            self.tracker_label.setText(tracker_state if not tracker_error else f"{tracker_state}: {tracker_error}")
            self.tracks_label.setText(
                f"{snapshot.status.get('track_count', 0)} active / "
                f"{snapshot.status.get('tracker_confirmed', 0)} confirmed / "
                f"{snapshot.status.get('tracker_lost', 0)} lost"
            )
            self.recording_workflow.observe(self._recording_status_from_stream(snapshot.status))
            if self.config.tracking_profile is not None:
                profile_matches = self.controller.validate_effective_profile(snapshot.status)
                if not profile_matches:
                    self.profile_label.setText(f"mismatch: {self.config.tracking_profile['profile_id']}")
                    if self.workflow.state in {LiveState.STARTING, LiveState.RUNNING, LiveState.DEGRADED}:
                        self.workflow.observe(LiveEvidence(fatal_error="board effective Profile Hash mismatch"))
                        self._schedule_emergency_stop()

        if not snapshot.status and snapshot.errors:
            self.status_available = False
            self.board_status_label.setText("unavailable")
        if snapshot.detections:
            self.latest_detections = snapshot.detections
            self.detections_label.setText(str(snapshot.detections.get("detection_count", "-")))
        if snapshot.tracks:
            self.latest_tracks = snapshot.tracks
        if snapshot.events:
            self.latest_events = snapshot.events
            self.intrusion_runtime_session_id, self.last_event_sequence = advance_event_cursor(
                self.latest_events, self.intrusion_runtime_session_id, self.last_event_sequence
            )
        self.latest_event_errors = list(snapshot.event_errors)
        self.latest_intrusion_overlay = select_intrusion_overlay(
            self.latest_stream_status,
            self.latest_events,
            self.latest_event_errors,
            self.config.stale_threshold_ms,
            fallback_profile=self.event_profile,
        )
        self.intrusion_label.setText(self.latest_intrusion_overlay.status)
        self.latest_overlay = select_overlay(
            self.latest_stream_status,
            self.latest_tracks,
            self.latest_detections,
            self.config.stale_threshold_ms,
        )
        self.overlay_source_label.setText(f"{self.latest_overlay.source}: {self.latest_overlay.reason}")
        self.metadata_age_label.setText(f"{self.latest_overlay.age_ms:.0f} ms")
        if snapshot.errors:
            self.stale_label.setText("metadata degraded")
            for error in snapshot.errors:
                self.event_log.error(error)
        elif self.latest_stream_frame is None:
            self.stale_label.setText("status readable; waiting for RTSP frame")
        for error in snapshot.event_errors:
            self.event_log.error(error)
        self._observe_live()

    def _schedule_emergency_stop(self) -> None:
        if self.emergency_stop_scheduled or self.live_stop_future is not None:
            return
        self.emergency_stop_scheduled = True
        self._request_live_stop(cleanup_only=True)

    def _side_channel_metadata_ready(self, detection_id: int) -> bool:
        if detection_id <= 0:
            return False
        update_time_ms = _int_value(
            self.latest_stream_status.get("latest_detection_time_ms"),
            self.latest_detections.get("update_time_ms"),
        )
        if update_time_ms <= 0:
            return True
        age_ms = max(0.0, time.time() * 1000.0 - update_time_ms)
        return age_ms <= self.config.stale_threshold_ms

    def _observe_live(self) -> None:
        video_id = _int_value(
            self.latest_stream_status.get("latest_rtsp_capture_frame_id"),
        )
        detection_id = _int_value(
            self.latest_stream_status.get("latest_detection_frame_id"),
            self.latest_detections.get("capture_frame_id"),
        )
        alignment = str(self.latest_stream_status.get("alignment_state", ""))
        clean_rtsp = (
            self.latest_stream_status.get("rtsp_clean_video") is True
            or self.latest_stream_status.get("alignment_source") == "rtsp-clean"
        )
        fatal = ""
        recorder_start_failed = self.recording_workflow.state in {RecorderState.FAILED, RecorderState.DISABLED}
        if self.workflow.state == LiveState.STARTING and self.recording_plan.enabled and recorder_start_failed:
            fatal = f"recording startup failed: {self.recording_workflow.error}"
        state = self.workflow.observe(
            LiveEvidence(
                board_status_readable=self.status_available,
                rtsp_frame_received=self.latest_stream_frame is not None,
                video_frame_id=video_id,
                detection_frame_id=detection_id,
                alignment_state=alignment,
                process_alive=True,
                transient_error=self.stream_error or ("; ".join(self.latest_stream_status.get("errors", [])) if isinstance(self.latest_stream_status.get("errors"), list) else ""),
                fatal_error=fatal,
                side_channel_metadata_ready=clean_rtsp and self._side_channel_metadata_ready(detection_id),
            )
        )
        if state == LiveState.ERROR and fatal:
            self._schedule_emergency_stop()
        self._set_state_label()

    def _apply_stream_frame(self, frame: StreamFrame) -> None:
        self.latest_stream_frame = frame
        self.stream_error = ""
        pixmap = QPixmap()
        if not pixmap.loadFromData(frame.jpeg, "JPG"):
            self._observe_live()
            return
        overlay = self._update_stream_overlay_state()
        if overlay.get("paint"):
            self._paint_overlay(pixmap)
        rotation = int(self.rotate_spin.value())
        if rotation:
            pixmap = pixmap.transformed(QTransform().rotate(rotation), Qt.SmoothTransformation)
        self.image_label.setPixmap(pixmap.scaled(self.image_label.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation))
        self.stale_label.setText("RTSP frame received")
        self._observe_live()

    def _update_stream_overlay_state(self) -> Dict[str, Any]:
        video_id = _int_value(self.latest_stream_status.get("latest_rtsp_capture_frame_id"))
        video_ts = _int_value(
            self.latest_stream_status.get("latest_rtsp_capture_frame_time_ms"),
            self.latest_stream_status.get("latest_rtsp_push_time_ms"),
        )
        self.latest_intrusion_overlay = select_intrusion_overlay(
            self.latest_stream_status,
            self.latest_events,
            self.latest_event_errors,
            self.config.stale_threshold_ms,
            fallback_profile=self.event_profile,
        )
        self.intrusion_label.setText(self.latest_intrusion_overlay.status)
        self.latest_overlay = select_overlay(
            self.latest_stream_status,
            self.latest_tracks,
            self.latest_detections,
            self.config.stale_threshold_ms,
        )
        metadata = self.latest_overlay.metadata
        detection_id = _int_value(metadata.get("capture_frame_id"))
        detection_ts = _int_value(metadata.get("capture_timestamp_ms"))
        lag_frames: Optional[int] = None
        lag_ms: Optional[int] = None
        state = "unavailable"
        paint = False
        boxes = self.latest_overlay.objects

        if video_id > 0 and detection_id <= 0:
            state = "none"
        elif video_id > 0 and detection_id > 0 and video_ts > 0 and detection_ts > 0:
            lag_frames = video_id - detection_id
            lag_ms = video_ts - detection_ts
            if lag_frames == 0:
                state, paint = "matched", True
            elif lag_ms > self.config.stale_threshold_ms:
                state = "stale"
            else:
                state, paint = "latest", True
        elif boxes:
            age = self.latest_overlay.age_ms
            if age > self.config.stale_threshold_ms:
                state, lag_ms = "stale", int(age)
            else:
                state, paint, lag_ms = "latest", True, int(age) if age else None
        if not boxes and not self.latest_intrusion_overlay.region:
            paint = False
        elif self.latest_intrusion_overlay.region:
            paint = True
        self.overlay_source_label.setText(f"{self.latest_overlay.source}: {self.latest_overlay.reason}")
        self.metadata_age_label.setText(f"{self.latest_overlay.age_ms:.0f} ms")
        return {"state": state, "paint": paint, "lag_frames": lag_frames, "lag_ms": lag_ms}

    def _paint_overlay(self, pixmap: QPixmap) -> None:
        boxes = self.latest_overlay.objects
        intrusion = self.latest_intrusion_overlay
        if not boxes and not intrusion.region and not intrusion.targets:
            return
        metadata = self.latest_overlay.metadata
        frame_w = self.latest_stream_frame.width if self.latest_stream_frame else 0
        frame_h = self.latest_stream_frame.height if self.latest_stream_frame else 0
        source_w = _positive_number(
            frame_w,
            int(
                _positive_number(
                    self.latest_events.get("source_width"),
                    int(_positive_number(metadata.get("source_width"), pixmap.width())),
                )
            ),
        )
        source_h = _positive_number(
            frame_h,
            int(
                _positive_number(
                    self.latest_events.get("source_height"),
                    int(_positive_number(metadata.get("source_height"), pixmap.height())),
                )
            ),
        )
        scale_x = pixmap.width() / source_w
        scale_y = pixmap.height() / source_h
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setFont(QFont("Sans Serif", 10, QFont.Bold))

        if intrusion.region:
            alarmed = intrusion_has_alarm(intrusion.targets)
            if intrusion.status == "enabled":
                region_color = QColor(230, 65, 65) if alarmed else QColor(245, 205, 45)
            else:
                region_color = QColor(150, 150, 150)
            painter.setPen(QPen(region_color, 3))
            rx1 = int(intrusion.region["x1"] * source_w * scale_x)
            ry1 = int(intrusion.region["y1"] * source_h * scale_y)
            rx2 = int(intrusion.region["x2"] * source_w * scale_x)
            ry2 = int(intrusion.region["y2"] * source_h * scale_y)
            painter.drawRect(rx1, ry1, max(1, rx2 - rx1), max(1, ry2 - ry1))

        event_track_ids = {
            target.get("track_id") for target in intrusion.targets if target.get("track_id") is not None
        }
        for box in boxes:
            if not isinstance(box, dict):
                continue
            if box.get("track_id") in event_track_ids:
                continue
            predicted = box.get("bbox_source") == "predicted"
            painter.setPen(QPen(QColor(255, 184, 76) if predicted else QColor(0, 220, 120), 2))
            x1 = int(float(box.get("x1", 0)) * scale_x)
            y1 = int(float(box.get("y1", 0)) * scale_y)
            x2 = int(float(box.get("x2", 0)) * scale_x)
            y2 = int(float(box.get("y2", 0)) * scale_y)
            painter.drawRect(x1, y1, max(1, x2 - x1), max(1, y2 - y1))
            if self.latest_overlay.source == "tracks":
                suffix = " pred" if predicted else ""
                label = f"{box.get('class', 'obj')} #{box.get('track_id', '?')}{suffix}"
            else:
                label = f"{box.get('class', 'obj')} {float(box.get('score', 0.0)):.2f}"
            painter.fillRect(x1, max(0, y1 - 18), max(80, len(label) * 8), 18, QColor(0, 0, 0, 170))
            painter.drawText(x1 + 3, max(14, y1 - 4), label)

        for target in intrusion.targets:
            bbox = target.get("bbox")
            if isinstance(bbox, list) and len(bbox) == 4:
                x1, y1, x2, y2 = [float(value) for value in bbox]
            else:
                x1 = float(target.get("x1", 0))
                y1 = float(target.get("y1", 0))
                x2 = float(target.get("x2", 0))
                y2 = float(target.get("y2", 0))
            color = QColor(230, 65, 65) if target.get("state") == "alarmed" else QColor(245, 205, 45)
            if intrusion.status != "enabled":
                color = QColor(150, 150, 150)
            x1 = int(x1 * scale_x)
            y1 = int(y1 * scale_y)
            x2 = int(x2 * scale_x)
            y2 = int(y2 * scale_y)
            painter.setPen(QPen(color, 3))
            painter.drawRect(x1, y1, max(1, x2 - x1), max(1, y2 - y1))
            label = intrusion_target_label(target)
            painter.fillRect(x1, max(0, y1 - 18), max(120, len(label) * 8), 18, QColor(0, 0, 0, 190))
            painter.drawText(x1 + 3, max(14, y1 - 4), label)
        painter.end()

    def _browse_replay(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Select local Replay Session")
        if path:
            self.replay_session_edit.setText(path)

    def _load_replay(self) -> None:
        if not self._is_replay_mode() or self.replay_workspace.controller is not None or self.replay_loading:
            return
        path = self.replay_session_edit.text().strip()
        if not path:
            self.event_log.error("Replay Session path is required")
            self.state_label.setText("Replay Error")
            return
        try:
            mode = self.replay_mode_combo.currentText()
            profile = self._effective_profile() if mode != "recorded" else None
        except Exception as exc:
            self.event_log.error(f"Replay setup rejected: {exc}")
            self.state_label.setText("Replay Error")
            return
        self.replay_loading = True
        self.state_label.setText("Loading Replay")
        self.replay_load_future = self.executor.submit(self._perform_replay_load, Path(path), mode, profile)
        self._update_controls()

    def _perform_replay_load(
        self, path: Path, mode: str, profile: Optional[Dict[str, Any]]
    ) -> Tuple[Any, Optional[Path], Optional[Dict[str, Any]]]:
        session = load_replay_session(path)
        output_path: Optional[Path] = None
        if mode != "recorded":
            if profile is None:
                raise ReplayError("rerun Profile is unavailable")
            validate_replay_profile(session, profile, mode)
            profile_file = Path(tempfile.gettempdir()) / f"rknn_detect_replay_profile_{profile_hash(profile)[7:]}.json"
            profile_file.write_text(json.dumps(profile, ensure_ascii=False, sort_keys=True), encoding="utf-8")
            output_path = Path(tempfile.mkdtemp(prefix="rknn_detect_host_replay_"))
            result = run_replay_command(
                self.config.replay_executable,
                session,
                mode,
                resolved_profile=profile_file,
                output=output_path,
            )
            if result.returncode != 0:
                raise ReplayError(result.stdout.strip() or "Replay Runner failed")
        return session, output_path, profile

    def _handle_replay_load(self) -> None:
        if not self.replay_load_future or not self.replay_load_future.done():
            return
        future = self.replay_load_future
        self.replay_load_future = None
        self.replay_loading = False
        try:
            session, output_path, profile = future.result()
            mode = self.replay_mode_combo.currentText()
            self.replay_controller = self.replay_workspace.load(session, mode)
            if output_path is not None:
                self.replay_controller.load_rerun_output(output_path)
            self._set_replay_layers(REPLAY_RERUN_LAYERS if mode != "recorded" else REPLAY_LAYERS,
                                    "replayed-tracks" if mode != "recorded" else "recorded-tracks")
            if mode == "recorded":
                self.profile_label.setText(f"{session.profile_id} {session.profile_hash}")
            elif profile is not None:
                self.profile_label.setText(f"{profile['profile_id']} {profile_hash(profile)}")
            self._apply_replay_record()
        except Exception as exc:
            self.event_log.error(f"Replay load failed: {exc}")
            self.state_label.setText("Replay Error")
        self._update_controls()

    def _advance_replay(self) -> None:
        if self.replay_controller is None or self.replay_workspace.state != "playing":
            return
        if self.replay_controller.index + 1 < len(self.replay_controller.session.records):
            self.replay_controller.step(1)
            self._apply_replay_record()
        else:
            self.replay_workspace.pause()
            self.replay_play_button.setText("Play")
        self.timer.setInterval(self.replay_controller.next_interval_ms())

    def _step_replay(self, delta: int) -> None:
        if self.replay_controller is None:
            return
        self.replay_workspace.step(delta)
        self._apply_replay_record()

    def _toggle_replay(self) -> None:
        if self.replay_controller is None:
            return
        if self.replay_workspace.state == "playing":
            self.replay_workspace.pause()
            self.replay_play_button.setText("Play")
        else:
            self.replay_workspace.play()
            self.replay_play_button.setText("Pause")
        self._apply_replay_record()

    def _apply_replay_record(self, *_args: Any) -> None:
        if self.replay_controller is None:
            return
        controller = self.replay_controller
        record = controller.record
        session = controller.session
        exact = image_path(session, record)
        pixmap = QPixmap()
        image_state = "Exact image"
        if exact and pixmap.load(str(exact)) and not pixmap.isNull():
            pass
        else:
            pixmap = QPixmap(max(1, session.width), max(1, session.height))
            pixmap.fill(QColor("#202020"))
            image_state = "No exact image"
        painter = QPainter(pixmap)
        painter.setFont(QFont("Sans Serif", 10, QFont.Bold))
        painter.setPen(QPen(QColor("#eeeeee"), 2))
        if image_state != "Exact image":
            painter.drawText(16, 28, "No exact image")
        layer = self.replay_layer_combo.currentText()
        boxes = observations_for_layer(record, layer)
        for box in boxes:
            if not isinstance(box, dict):
                continue
            bbox = box.get("bbox")
            if isinstance(bbox, list) and len(bbox) == 4:
                x1, y1, x2, y2 = [int(float(value)) for value in bbox]
            else:
                x1 = int(float(box.get("x1", 0)))
                y1 = int(float(box.get("y1", 0)))
                x2 = int(float(box.get("x2", 0)))
                y2 = int(float(box.get("y2", 0)))
            color = QColor(255, 184, 76) if layer == "rejected" else QColor(0, 220, 120)
            painter.setPen(QPen(color, 2))
            painter.drawRect(x1, y1, max(1, x2 - x1), max(1, y2 - y1))
            reasons = ",".join(box.get("rejection_reasons", []))
            label = f"{box.get('class_name', box.get('class', 'obj'))} {reasons}".strip()
            painter.drawText(x1 + 2, max(14, y1 - 3), label)
        painter.end()
        rotation = int(self.rotate_spin.value())
        if rotation:
            pixmap = pixmap.transformed(QTransform().rotate(rotation), Qt.SmoothTransformation)
        self.image_label.setPixmap(pixmap.scaled(self.image_label.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation))
        state = "playing" if self.replay_workspace.state == "playing" else "paused"
        self.state_label.setText(f"Replay {state} {controller.index + 1}/{len(session.records)}")
        self.stale_label.setText(image_state)
        self.overlay_source_label.setText(f"{layer} / {controller.mode}")
        self.metadata_age_label.setText(f"capture {record.get('capture_timestamp_ms', '-')}")

    def _set_state_label(self) -> None:
        if self._is_replay_mode() and (self.replay_controller is not None or self.replay_loading):
            return
        self.state_label.setText(self.workflow.state.value)
        self.recording_state_label.setText(self.recording_workflow.state.value)
        self._update_controls()

    def _update_controls(self, *_args: Any) -> None:
        live = self._is_live_mode()
        replay = self._is_replay_mode()
        live_idle_or_error = self.workflow.state in {LiveState.IDLE, LiveState.ERROR}
        settings_enabled = live and live_idle_or_error
        self.mode_combo.setEnabled(self.workflow.state == LiveState.IDLE and self.replay_controller is None and not self.replay_loading)
        self.start_button.setEnabled(live and live_idle_or_error and not self.replay_loading)
        self.stop_button.setEnabled(
            (live and self.workflow.state in {LiveState.STARTING, LiveState.RUNNING, LiveState.DEGRADED, LiveState.ERROR})
            or (replay and self.replay_controller is not None)
        )
        if self.live_stop_future is not None and not self.live_stop_future.done():
            self.stop_button.setEnabled(False)
        self.profile_combo.setEnabled(settings_enabled)
        self.camera_combo.setEnabled(settings_enabled)
        self.roi_enabled_check.setEnabled(settings_enabled)
        for spin in self.roi_spins.values():
            spin.setEnabled(settings_enabled and self.roi_enabled_check.isChecked())
        self.recording_combo.setEnabled(settings_enabled)
        self.load_replay_button.setEnabled(replay and self.replay_controller is None and not self.replay_loading)
        self.replay_browse_button.setEnabled(replay and self.replay_controller is None and not self.replay_loading)
        self.replay_session_edit.setEnabled(replay and self.replay_controller is None and not self.replay_loading)
        self.replay_mode_combo.setEnabled(replay and self.replay_controller is None and not self.replay_loading)
        replay_loaded = replay and self.replay_controller is not None
        self.replay_layer_combo.setEnabled(replay_loaded)
        self.replay_previous_button.setEnabled(replay_loaded)
        self.replay_next_button.setEnabled(replay_loaded)
        self.replay_play_button.setEnabled(replay_loaded)
        if replay_loaded:
            self.stop_button.setText("Stop")
        else:
            self.stop_button.setText("Stop")

    def _append_logs(self) -> None:
        current_lines = self.log_text.toPlainText().splitlines()
        already = len(current_lines)
        for entry in self.event_log.entries[already:]:
            self.log_text.append(f"{entry.timestamp} [{entry.level}] {entry.message}")


def _fmt(value: Any) -> str:
    if isinstance(value, (float, int)):
        return f"{value:.3f}"
    return "-"


def _positive_number(value: Any, fallback: int) -> float:
    if isinstance(value, (float, int)) and value > 0:
        return float(value)
    return float(fallback)


def _int_value(*values: Any) -> int:
    for value in values:
        if isinstance(value, bool):
            continue
        if isinstance(value, int) and value > 0:
            return value
        if isinstance(value, float) and value > 0:
            return int(value)
        if isinstance(value, str):
            try:
                parsed = int(value)
            except ValueError:
                continue
            if parsed > 0:
                return parsed
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="RK3568 smart-monitor Host Viewer GUI")
    parser.add_argument("--board-host", default=None, help="board IP or hostname")
    parser.add_argument("--board-user", default=None, help="SSH user")
    parser.add_argument("--remote-project-dir", default=None, help="project directory on the board")
    parser.add_argument("--model-path", default=None, help="RKNN model path on the board")
    parser.add_argument("--video-node", default=None, help="V4L2 video node on the board")
    parser.add_argument("--stream-port", type=int, default=None, help="HTTP status port")
    parser.add_argument("--rtsp-port", type=int, default=None, help="RTSP port")
    parser.add_argument("--rtsp-path", default=None, help="RTSP mount path")
    parser.add_argument("--smoke", action="store_true", help="initialize the complete Qt viewer headlessly")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    app = QApplication(sys.argv[:1])
    config = ViewerConfig().with_overrides(
        board_host=args.board_host,
        board_user=args.board_user,
        remote_project_dir=args.remote_project_dir,
        model_path=args.model_path,
        video_node=args.video_node,
        stream_port=args.stream_port,
        rtsp_port=args.rtsp_port,
        rtsp_path=args.rtsp_path,
    )
    if args.smoke:
        window = HostViewerWindow(config)
        app.processEvents()
        window.close()
        window.deleteLater()
        app.processEvents()
        app.sendPostedEvents()
        return 0
    window = HostViewerWindow(config)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
