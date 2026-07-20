import os
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from tools.host_viewer.config import ViewerConfig
from tools.host_viewer.gui import HostViewerWindow
from tools.host_viewer.recording import RecordingPlan, RecordingWorkflow
from tools.host_viewer.tracking_profile import profile_hash


class HostViewerGuiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    @classmethod
    def tearDownClass(cls):
        cls.app.quit()
        cls.app.processEvents()
        cls.app = None

    def test_default_operator_surface_is_live_idle_without_connect(self):
        window = HostViewerWindow(ViewerConfig())
        try:
            self.assertEqual(["Live", "Replay"], [window.mode_combo.itemText(i) for i in range(window.mode_combo.count())])
            self.assertFalse(hasattr(window, "connect_button"))
            self.assertEqual("Idle", window.workflow.state.value)
            self.assertEqual("default-general", window.profile_combo.currentData())
            self.assertFalse(window.roi_enabled_check.isChecked())
            self.assertEqual("off", window.recording_combo.currentData())
            self.assertEqual(0, window.rotate_spin.value())
            self.assertTrue(window.start_button.isEnabled())
            self.assertFalse(window.stop_button.isEnabled())
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()

    def test_live_settings_are_locked_after_starting(self):
        window = HostViewerWindow(ViewerConfig())
        try:
            token = window.workflow.begin_start()
            window.live_start_token = token
            window._update_controls()
            self.assertFalse(window.profile_combo.isEnabled())
            self.assertFalse(window.recording_combo.isEnabled())
            self.assertFalse(window.mode_combo.isEnabled())
            self.assertTrue(window.stop_button.isEnabled())
            window.workflow.request_stop()
            window.workflow.complete_stop()
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()

    def test_turning_roi_off_rebuilds_canonical_profile_for_restart(self):
        window = HostViewerWindow(ViewerConfig())
        try:
            window.profile_combo.setCurrentIndex(window.profile_combo.findData("indoor-person"))
            window.roi_enabled_check.setChecked(True)
            for key, value in {"x1": 0.25, "y1": 0.25, "x2": 0.75, "y2": 0.75}.items():
                window.roi_spins[key].setValue(value)
            window._prepare_live_config()

            window.roi_enabled_check.setChecked(False)
            window._prepare_live_config()
            profile = window.config.tracking_profile
            args = window.controller._build_stream_args()

            self.assertEqual([0], profile["observation"]["allowed_class_ids"])
            self.assertEqual(
                {"enabled": False, "x1": 0.0, "y1": 0.0, "x2": 1.0, "y2": 1.0},
                profile["observation"]["roi"],
            )
            self.assertEqual(profile_hash(window._selected_preset().resolved), profile_hash(profile))
            self.assertEqual("disabled", args[args.index("--tracker-roi") + 1])
            self.assertEqual(profile_hash(profile), args[args.index("--profile-hash") + 1])
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()

    def test_recording_stop_copies_finalized_session_to_project_recordings(self):
        window = HostViewerWindow(ViewerConfig())
        try:
            plan = RecordingPlan.create("metadata", runtime_session_id="runtime-1", recording_session_id="recording-1")
            window.recording_plan = plan
            window.recording_workflow = RecordingWorkflow(plan)
            with TemporaryDirectory() as temp_dir:
                window.repo_root = Path(temp_dir)
                copied = Path(temp_dir) / "recordings" / "recording-1"
                with patch.object(window.controller, "stop_stream") as stop_stream, patch.object(
                    window.controller,
                    "wait_for_recording_terminal",
                    return_value={"state": "finalized", "session_id": "recording-1"},
                ), patch.object(window.controller, "copy_recording_session", return_value=copied) as copy_session:
                    status = window._perform_live_stop(plan)

                stop_stream.assert_called_once_with()
                copy_session.assert_called_once_with(plan, Path(temp_dir) / "recordings")
                self.assertEqual(str(copied), status["local_session_path"])
                window._apply_recording_stop_status(status, context="test")
                self.assertEqual(str(copied), window.replay_session_edit.text())
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()

    def test_recording_stop_does_not_copy_nonfinal_session(self):
        window = HostViewerWindow(ViewerConfig())
        try:
            plan = RecordingPlan.create("metadata", runtime_session_id="runtime-1", recording_session_id="recording-1")
            window.recording_plan = plan
            window.recording_workflow = RecordingWorkflow(plan)
            with patch.object(window.controller, "stop_stream"), patch.object(
                window.controller,
                "wait_for_recording_terminal",
                return_value={"state": "interrupted", "session_id": "recording-1"},
            ), patch.object(window.controller, "copy_recording_session") as copy_session:
                status = window._perform_live_stop(plan)

            copy_session.assert_not_called()
            self.assertNotIn("local_session_path", status)
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
