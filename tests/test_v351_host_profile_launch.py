import json
import unittest
from tempfile import TemporaryDirectory
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools.host_viewer.config import ViewerConfig
from tools.host_viewer.controller import BoardController
from tools.host_viewer.recording import RecordingPlan
from tools.host_viewer.tracking_profile import profile_hash


FIXTURE = Path(__file__).parent / "data" / "v351_profile_source.json"


class HostProfileLaunchTest(unittest.TestCase):
    def test_default_launch_does_not_add_profile_arguments(self) -> None:
        args = BoardController(ViewerConfig())._build_stream_args()
        self.assertNotIn("--profile-id", args)
        self.assertEqual("0.500000", args[args.index("--box-thresh") + 1])

    def test_resolved_profile_expands_to_explicit_arguments(self) -> None:
        source = json.loads(FIXTURE.read_text(encoding="utf-8"))
        config = ViewerConfig(tracking_profile=source)
        args = BoardController(config)._build_stream_args()
        self.assertEqual("indoor-person", args[args.index("--profile-id") + 1])
        self.assertEqual(profile_hash(config.tracking_profile), args[args.index("--profile-hash") + 1])
        self.assertEqual("0", args[args.index("--tracker-class-ids") + 1])
        self.assertEqual("24.000000", args[args.index("--tracker-min-width") + 1])
        self.assertEqual("disabled", args[args.index("--tracker-roi") + 1])
        self.assertEqual("0.350000", args[args.index("--tracker-low-thresh") + 1])

    def test_invalid_profile_fails_before_command_construction(self) -> None:
        with self.assertRaisesRegex(ValueError, "must be unique"):
            ViewerConfig(tracking_profile={
                "profile_id": "bad", "observation": {"allowed_class_ids": [0, 0]}})

    def test_controller_reports_board_effective_mismatch(self) -> None:
        source = json.loads(FIXTURE.read_text(encoding="utf-8"))
        controller = BoardController(ViewerConfig(tracking_profile=source))
        self.assertFalse(controller.validate_effective_profile({"profile_id": "other", "profile_hash": "bad"}))
        self.assertEqual(2, len(controller.log.entries))

    def test_recording_plan_is_expanded_into_stream_start_args(self) -> None:
        plan = RecordingPlan.create("sampled", runtime_session_id="runtime-1", recording_session_id="recording-1")
        config = ViewerConfig(
            tracking_profile=json.loads(FIXTURE.read_text(encoding="utf-8")),
            recording_plan=plan,
        )
        args = BoardController(config)._build_stream_args()
        self.assertIn("--record-analysis", args)
        self.assertEqual("runtime-1", args[args.index("--runtime-session-id") + 1])
        self.assertEqual("recording-1", args[args.index("--recording-session-id") + 1])
        self.assertEqual("sampled", args[args.index("--record-frame-mode") + 1])

    def test_recording_preflight_checks_remote_root_and_session_collision(self) -> None:
        plan = RecordingPlan.create("metadata", runtime_session_id="runtime-1", recording_session_id="recording-1")
        controller = BoardController(ViewerConfig(recording_plan=plan))
        with patch("tools.host_viewer.controller.subprocess.run") as run:
            run.return_value = SimpleNamespace(returncode=0, stdout="", stderr="")
            self.assertTrue(controller.check_recording_preflight())

        command = run.call_args.args[0][-1]
        self.assertIn("mkdir -p", command)
        self.assertIn("recording-1", command)
        self.assertIn("test -w", command)

    def test_copy_recording_session_to_local_project_root(self) -> None:
        plan = RecordingPlan.create("metadata", runtime_session_id="runtime-1", recording_session_id="recording-1")
        controller = BoardController(ViewerConfig(recording_plan=plan))

        def fake_copy(command, **_kwargs):
            staging_root = Path(command[-1])
            session = staging_root / plan.recording_session_id
            session.mkdir()
            (session / "manifest.json").write_text("{}", encoding="utf-8")
            (session / "records-000001.jsonl").write_text("{}\n", encoding="utf-8")
            (session / "session-status.json").write_text('{"state":"finalized"}\n', encoding="utf-8")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with TemporaryDirectory() as temp_dir:
            local_root = Path(temp_dir) / "recordings"
            with patch("tools.host_viewer.controller.subprocess.run", side_effect=fake_copy) as run:
                copied = controller.copy_recording_session(plan, local_root)

            expected = local_root / plan.recording_session_id
            self.assertEqual(expected, copied)
            self.assertTrue((expected / "manifest.json").is_file())
            command = run.call_args.args[0]
            self.assertIn("-r", command)
            self.assertIn(f"{controller.config.ssh_target}:/root/project/rknn_detect/recordings/recording-1", command)

    def test_copy_recording_session_rejects_incomplete_staging(self) -> None:
        plan = RecordingPlan.create("metadata", runtime_session_id="runtime-1", recording_session_id="recording-1")
        controller = BoardController(ViewerConfig(recording_plan=plan))

        def fake_copy(command, **_kwargs):
            staging_root = Path(command[-1])
            session = staging_root / plan.recording_session_id
            session.mkdir()
            (session / "manifest.json").write_text("{}", encoding="utf-8")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with TemporaryDirectory() as temp_dir:
            local_root = Path(temp_dir) / "recordings"
            with patch("tools.host_viewer.controller.subprocess.run", side_effect=fake_copy):
                with self.assertRaisesRegex(RuntimeError, "missing"):
                    controller.copy_recording_session(plan, local_root)

            self.assertFalse((local_root / plan.recording_session_id).exists())
            self.assertEqual([], list(local_root.iterdir()))


if __name__ == "__main__":
    unittest.main()
