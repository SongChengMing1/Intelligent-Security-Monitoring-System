import json
import unittest
import tempfile
import shutil
from pathlib import Path

from tools.host_viewer.replay import (
    REPLAY_LAYERS,
    REPLAY_RERUN_LAYERS,
    ReplayController,
    ReplayError,
    ReplayWorkspace,
    blank_canvas_size,
    image_path,
    load_replay_session,
    observations_for_layer,
    validate_replay_profile,
)
from tools.host_viewer.tracking_profile import resolve_tracking_profile


SESSION = Path(__file__).parent / "data" / "v351_session"


class HostViewerReplayTest(unittest.TestCase):
    def test_session_loads_and_uses_capture_order(self) -> None:
        session = load_replay_session(SESSION)
        self.assertEqual([1], [record["capture_frame_id"] for record in session.records])
        self.assertEqual((640, 480), blank_canvas_size(session))
        self.assertIsNone(image_path(session, session.records[0]))

    def test_layers_distinguish_high_pass_and_tracker_input(self) -> None:
        session = load_replay_session(SESSION)
        record = session.records[0]
        self.assertEqual([1], [item["observation_id"] for item in observations_for_layer(record, "high-pass")])
        self.assertEqual([1], [item["observation_id"] for item in observations_for_layer(record, "tracker-input")])
        self.assertEqual([], observations_for_layer(record, "rejected"))
        self.assertIn("recorded-tracks", REPLAY_LAYERS)
        self.assertIn("replayed-tracks", REPLAY_RERUN_LAYERS)

    def test_replay_workspace_loads_paused_and_stop_unloads(self) -> None:
        session = load_replay_session(SESSION)
        workspace = ReplayWorkspace()
        self.assertEqual("empty", workspace.state)
        workspace.load(session)
        self.assertEqual("paused", workspace.state)
        workspace.play()
        self.assertEqual("playing", workspace.state)
        workspace.pause()
        self.assertEqual("paused", workspace.state)
        workspace.stop()
        self.assertEqual("empty", workspace.state)
        self.assertIsNone(workspace.controller)

    def test_rerun_rejects_detector_threshold_mismatch(self) -> None:
        session = load_replay_session(SESSION)
        profile = resolve_tracking_profile({
            "profile_id": "indoor-person",
            "detector": {"high_threshold": 0.7},
        })
        with self.assertRaisesRegex(ReplayError, "detector"):
            validate_replay_profile(session, profile, "tracker-rerun")

    def test_tracker_rerun_keeps_detector_profile_comparable(self) -> None:
        session = load_replay_session(SESSION)
        profile = resolve_tracking_profile({"profile_id": "indoor-person", "tracker": {"lost_timeout_ms": 1200}})
        validate_replay_profile(session, profile, "tracker-rerun")

    def test_policy_rerun_output_updates_policy_layers(self) -> None:
        session = load_replay_session(SESSION)
        with tempfile.TemporaryDirectory() as root:
            output = Path(root)
            (output / "tracks.jsonl").write_text(
                json.dumps({"capture_frame_id": 1, "objects": [{"track_id": 7}]}) + "\n",
                encoding="utf-8",
            )
            (output / "policy-decisions.jsonl").write_text(
                json.dumps({"capture_frame_id": 1, "admitted_observation_ids": []}) + "\n",
                encoding="utf-8",
            )
            controller = ReplayController(session)
            controller.mode = "policy-tracker-rerun"
            controller.load_rerun_output(output)

            self.assertEqual([], observations_for_layer(controller.record, "tracker-input"))
            self.assertEqual([1], [item["observation_id"] for item in observations_for_layer(controller.record, "rejected")])
            self.assertEqual([7], [item["track_id"] for item in observations_for_layer(controller.record, "replayed-tracks")])

    def test_replay_controller_steps_and_uses_capture_timing(self) -> None:
        session = load_replay_session(SESSION)
        controller = ReplayController(session)
        controller.speed = 2.0
        self.assertEqual(1000, controller.next_interval_ms())
        self.assertEqual(1, controller.record["capture_frame_id"])
        self.assertEqual("recorded", controller.mode)

    def test_missing_manifest_is_rejected(self) -> None:
        with self.assertRaises(ReplayError):
            load_replay_session(SESSION / "missing")

    def test_truncated_final_record_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            copy = Path(root) / "session"
            shutil.copytree(SESSION, copy)
            records = next(copy.glob("records-*.jsonl"))
            records.write_text(records.read_text(encoding="utf-8").rstrip("\n"), encoding="utf-8")
            with self.assertRaisesRegex(ReplayError, "truncated"):
                load_replay_session(copy)


if __name__ == "__main__":
    unittest.main()
