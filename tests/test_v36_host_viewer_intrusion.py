from __future__ import annotations

import json
import os
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from tools.host_viewer.cli import apply_default_event_profile
from tools.host_viewer.config import ViewerConfig
from tools.host_viewer.controller import BoardController
from tools.host_viewer.event_profile import load_event_profile
from tools.host_viewer.intrusion_overlay import (
    advance_event_cursor,
    intrusion_has_alarm,
    intrusion_target_label,
    select_intrusion_overlay,
)
from tools.host_viewer.gui import HostViewerWindow
from tools.host_viewer.stream import StreamFrame, StreamSnapshot


PROFILE_PATH = Path(__file__).parents[1] / "profiles" / "events" / "camera0.json"


class EventProfileTest(unittest.TestCase):
    def test_default_profile_is_left_half_person_rule(self) -> None:
        profile = load_event_profile(PROFILE_PATH, "camera0")
        self.assertTrue(profile["enabled"])
        self.assertEqual([0], profile["rule"]["class_ids"])
        self.assertEqual(
            {"type": "rectangle", "x1": 0.0, "y1": 0.0, "x2": 0.5, "y2": 1.0},
            profile["rule"]["region"],
        )
        self.assertEqual(5000, profile["rule"]["dwell_ms"])

    def test_invalid_camera_is_rejected_before_board_launch(self) -> None:
        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["camera_id"] = "camera1"
        with self.assertRaisesRegex(ValueError, "does not match"):
            load_event_profile_from_source(source)

    def test_invalid_schema_and_missing_fields_are_rejected(self) -> None:
        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["event_profile_schema_version"] = 2
        with self.assertRaisesRegex(ValueError, "unsupported"):
            load_event_profile_from_source(source)

        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        del source["rule"]["dwell_ms"]
        with self.assertRaisesRegex(ValueError, "missing field"):
            load_event_profile_from_source(source)

    def test_invalid_region_and_rule_parameters_are_rejected(self) -> None:
        invalid_sources = []
        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["rule"]["region"]["x2"] = 1.2
        invalid_sources.append((source, "ordered normalized rectangle"))

        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["rule"]["dwell_ms"] = 0
        invalid_sources.append((source, "positive integer"))

        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["rule"]["boundary_hysteresis_px"] = -1
        invalid_sources.append((source, "non-negative"))

        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["rule"]["prediction_grace_ms"] = -1
        invalid_sources.append((source, "non-negative integer"))

        for invalid_source, message in invalid_sources:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    load_event_profile_from_source(invalid_source)


def load_event_profile_from_source(source: dict) -> dict:
    from tools.host_viewer.event_profile import resolve_event_profile

    return resolve_event_profile(source, "camera0")


class EventLaunchTest(unittest.TestCase):
    def test_controller_without_resolved_event_profile_keeps_events_disabled(self) -> None:
        args = BoardController(ViewerConfig())._build_stream_args()
        self.assertNotIn("--intrusion-enabled", args)
        self.assertNotIn("--intrusion-disabled", args)

    def test_cli_default_event_profile_is_loaded_before_stream_launch(self) -> None:
        config = ViewerConfig()
        apply_default_event_profile(config)
        args = BoardController(config)._build_stream_args()
        self.assertIn("--intrusion-enabled", args)
        self.assertEqual("camera0", args[args.index("--intrusion-camera-id") + 1])
        self.assertEqual(
            "0.000000,0.000000,0.500000,1.000000",
            args[args.index("--intrusion-region") + 1],
        )

    def test_event_profile_is_expanded_to_explicit_board_arguments(self) -> None:
        profile = load_event_profile(PROFILE_PATH, "camera0")
        args = BoardController(ViewerConfig(event_profile=profile))._build_stream_args()
        self.assertIn("--intrusion-enabled", args)
        self.assertEqual("camera0", args[args.index("--intrusion-camera-id") + 1])
        self.assertEqual("0.000000,0.000000,0.500000,1.000000", args[args.index("--intrusion-region") + 1])
        self.assertEqual("5000", args[args.index("--intrusion-dwell-ms") + 1])

    def test_no_event_profile_does_not_send_event_arguments(self) -> None:
        args = BoardController(ViewerConfig(event_profile_path=None))._build_stream_args()
        self.assertNotIn("--intrusion-enabled", args)
        self.assertNotIn("--intrusion-disabled", args)

    def test_explicit_disabled_profile_preserves_disabled_board_mode(self) -> None:
        source = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        source["enabled"] = False
        profile = load_event_profile_from_source(source)
        args = BoardController(ViewerConfig(event_profile=profile))._build_stream_args()
        self.assertIn("--intrusion-disabled", args)
        self.assertNotIn("--intrusion-enabled", args)
        self.assertNotIn("--intrusion-region", args)


class IntrusionOverlayTest(unittest.TestCase):
    def test_event_cursor_deduplicates_sequences_within_runtime_session(self) -> None:
        session, sequence = advance_event_cursor(
            {"runtime_session_id": "runtime-1", "event_sequence": 2}, "runtime-1", 1
        )
        self.assertEqual(("runtime-1", 2), (session, sequence))
        self.assertEqual(
            ("runtime-1", 2),
            advance_event_cursor(
                {"runtime_session_id": "runtime-1", "event_sequence": 1}, session, sequence
            ),
        )
        self.assertEqual(
            ("runtime-2", 1),
            advance_event_cursor(
                {"runtime_session_id": "runtime-2", "event_sequence": 1}, session, sequence
            ),
        )

    def test_enabled_overlay_exposes_dwell_progress_and_alarm_label(self) -> None:
        status = {"intrusion_enabled": True}
        events = {
            "enabled": True,
            "state": "running",
            "update_time_ms": 9900,
            "event_sequence": 1,
            "region": {"x1": 0.0, "y1": 0.0, "x2": 0.5, "y2": 1.0},
            "in_region_targets": [
                {
                    "track_id": 7,
                    "class_name": "person",
                    "state": "dwelling",
                    "dwell_ms": 3200,
                    "threshold_ms": 5000,
                    "bbox": [100.0, 80.0, 180.0, 300.0],
                }
            ],
        }
        selection = select_intrusion_overlay(status, events, [], 1000, now_ms=10000)
        self.assertEqual("enabled", selection.status)
        self.assertEqual("person #7 3.2 / 5.0s", intrusion_target_label(selection.targets[0]))

        events["in_region_targets"][0]["state"] = "alarmed"
        selection = select_intrusion_overlay(status, events, [], 1000, now_ms=10000)
        self.assertEqual("INTRUSION person #7", intrusion_target_label(selection.targets[0]))

    def test_stale_and_endpoint_failure_are_not_reported_as_enabled(self) -> None:
        status = {"intrusion_enabled": True}
        events = {"enabled": True, "state": "running", "update_time_ms": 8000}
        self.assertEqual(
            "stale",
            select_intrusion_overlay(status, events, [], 1000, now_ms=10000).status,
        )
        self.assertEqual(
            "unavailable",
            select_intrusion_overlay(status, events, ["events.json unavailable"], 1000, now_ms=10000).status,
        )
        fresh_events = {"enabled": True, "state": "running", "update_time_ms": 9900}
        unavailable_status = {"intrusion_enabled": True, "intrusion_state": "unavailable"}
        self.assertEqual(
            "unavailable",
            select_intrusion_overlay(unavailable_status, fresh_events, [], 1000, now_ms=10000).status,
        )
        self.assertEqual(
            "unavailable",
            select_intrusion_overlay(status, fresh_events, ["events.json unavailable"], 1000, now_ms=10000).status,
        )
        self.assertEqual(
            "enabled",
            select_intrusion_overlay(status, fresh_events, [], 1000, now_ms=10000).status,
        )

    def test_multiple_targets_keep_independent_video_labels(self) -> None:
        status = {"intrusion_enabled": True}
        events = {
            "enabled": True,
            "state": "running",
            "update_time_ms": 9900,
            "event_sequence": 2,
            "region": {"x1": 0.0, "y1": 0.0, "x2": 0.5, "y2": 1.0},
            "in_region_targets": [
                {
                    "track_id": 7,
                    "class_name": "person",
                    "state": "dwelling",
                    "dwell_ms": 3200,
                    "threshold_ms": 5000,
                    "bbox": [100.0, 80.0, 180.0, 300.0],
                },
                {
                    "track_id": 8,
                    "class_name": "person",
                    "state": "alarmed",
                    "dwell_ms": 5000,
                    "threshold_ms": 5000,
                    "bbox": [200.0, 80.0, 280.0, 300.0],
                },
            ],
        }
        selection = select_intrusion_overlay(status, events, [], 1000, now_ms=10000)
        self.assertEqual("enabled", selection.status)
        self.assertEqual(2, len(selection.targets))
        self.assertEqual(
            ["person #7 3.2 / 5.0s", "INTRUSION person #8"],
            [intrusion_target_label(target) for target in selection.targets],
        )
        self.assertTrue(intrusion_has_alarm(selection.targets))


class IntrusionGuiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.app.quit()
        cls.app.processEvents()
        cls.app = None

    def test_gui_shows_only_intrusion_capability_on_right_and_paints_video_state(self) -> None:
        window = HostViewerWindow(ViewerConfig())
        try:
            now_ms = int(time.time() * 1000)
            window._apply_stream_snapshot(
                StreamSnapshot(
                    status={
                        "state": "running",
                        "intrusion_enabled": True,
                        "intrusion_state": "running",
                    },
                    events={
                        "enabled": True,
                        "state": "running",
                        "update_time_ms": now_ms,
                        "source_width": 640,
                        "source_height": 480,
                        "region": {"x1": 0.0, "y1": 0.0, "x2": 0.5, "y2": 1.0},
                        "in_region_targets": [
                            {
                                "track_id": 7,
                                "class_name": "person",
                                "state": "dwelling",
                                "dwell_ms": 3200,
                                "threshold_ms": 5000,
                                "bbox": [100.0, 80.0, 180.0, 300.0],
                            }
                        ],
                    },
                )
            )
            self.assertEqual("enabled", window.intrusion_label.text())
            overlay = window._update_stream_overlay_state()
            self.assertTrue(overlay["paint"])
            self.assertEqual("enabled", window.latest_intrusion_overlay.status)
            self.assertEqual("person #7 3.2 / 5.0s", intrusion_target_label(window.latest_intrusion_overlay.targets[0]))
        finally:
            window.close()
            window.deleteLater()
            self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
