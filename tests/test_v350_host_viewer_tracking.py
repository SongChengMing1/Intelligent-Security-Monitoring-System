from __future__ import annotations

import unittest
from unittest.mock import patch

from tools.host_viewer.config import ViewerConfig
from tools.host_viewer.stream import fetch_stream_snapshot
from tools.host_viewer.tracking_overlay import metadata_age_ms, select_overlay


NOW_MS = 10_000.0


def detection(update_time_ms: int = 9_900) -> dict:
    return {
        "state": "running",
        "update_time_ms": update_time_ms,
        "objects": [{"class": "person", "score": 0.8, "x1": 1, "y1": 2, "x2": 3, "y2": 4}],
    }


def tracks(state: str = "running", update_time_ms: int = 9_900, track_state: str = "confirmed") -> dict:
    return {
        "state": state,
        "update_time_ms": update_time_ms,
        "objects": [
            {
                "track_id": 7,
                "class": "person",
                "track_state": track_state,
                "bbox_source": "observed",
                "x1": 1,
                "y1": 2,
                "x2": 3,
                "y2": 4,
            }
        ],
    }


class TrackingOverlaySelectionTest(unittest.TestCase):
    def test_confirmed_fresh_track_is_preferred(self) -> None:
        selection = select_overlay(
            {"tracker_enabled": True, "tracker_state": "running"},
            tracks(),
            detection(),
            1_000,
            NOW_MS,
        )
        self.assertEqual("tracks", selection.source)
        self.assertEqual(7, selection.objects[0]["track_id"])

    def test_tracker_metadata_controls_overlay_fallback(self) -> None:
        cases = (
            (tracks(track_state="tentative"), "none"),
            ({"state": "running", "update_time_ms": 9_900, "objects": []}, "none"),
            (tracks(state="error"), "detections"),
            (tracks(update_time_ms=8_000), "none"),
        )
        for track_metadata, expected_source in cases:
            with self.subTest(track_metadata=track_metadata):
                selection = select_overlay(
                    {"tracker_enabled": True, "tracker_state": "running"},
                    track_metadata,
                    detection(),
                    1_000,
                    NOW_MS,
                )
                self.assertEqual(expected_source, selection.source)

    def test_running_tracker_with_no_confirmed_tracks_hides_raw_detections(self) -> None:
        selection = select_overlay(
            {"tracker_enabled": True, "tracker_state": "running"},
            {"state": "running", "update_time_ms": 9_900, "objects": []},
            detection(),
            1_000,
            NOW_MS,
        )

        self.assertEqual("none", selection.source)
        self.assertEqual([], selection.objects)

    def test_disabled_tracker_uses_detection_and_stale_detection_is_hidden(self) -> None:
        fallback = select_overlay(
            {"tracker_enabled": False, "tracker_state": "disabled"},
            {},
            detection(),
            1_000,
            NOW_MS,
        )
        self.assertEqual("detections", fallback.source)

        hidden = select_overlay({}, {}, detection(update_time_ms=8_000), 1_000, NOW_MS)
        self.assertEqual("none", hidden.source)
        self.assertEqual([], hidden.objects)

    def test_negative_clock_skew_does_not_create_negative_age(self) -> None:
        self.assertEqual(0.0, metadata_age_ms({"update_time_ms": 11_000}, NOW_MS))


class _FakeResponse:
    def __init__(self, payload: str) -> None:
        self.payload = payload.encode("utf-8")

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def read(self) -> bytes:
        return self.payload


class StreamSnapshotTest(unittest.TestCase):
    def test_status_detection_and_tracks_are_fetched_independently(self) -> None:
        payloads = {
            "status.json": '{"state":"running"}',
            "detections.json": '{"detection_count":1}',
            "tracks.json": '{"track_count":1}',
            "events.json": '{"state":"disabled","enabled":false}',
        }

        def open_url(url: str, timeout: float) -> _FakeResponse:
            del timeout
            return _FakeResponse(payloads[url.rsplit("/", 1)[-1]])

        with patch("tools.host_viewer.stream._DIRECT_OPENER.open", side_effect=open_url):
            snapshot = fetch_stream_snapshot(ViewerConfig())

        self.assertEqual("running", snapshot.status["state"])
        self.assertEqual(1, snapshot.detections["detection_count"])
        self.assertEqual(1, snapshot.tracks["track_count"])
        self.assertFalse(snapshot.events["enabled"])
        self.assertEqual([], snapshot.errors)
        self.assertEqual([], snapshot.tracking_errors)

    def test_track_endpoint_failure_does_not_mark_stream_unavailable(self) -> None:
        def open_url(url: str, timeout: float) -> _FakeResponse:
            del timeout
            if url.endswith("tracks.json"):
                raise OSError("not supported")
            return _FakeResponse("{}")

        with patch("tools.host_viewer.stream._DIRECT_OPENER.open", side_effect=open_url):
            snapshot = fetch_stream_snapshot(ViewerConfig())

        self.assertEqual([], snapshot.errors)
        self.assertEqual(1, len(snapshot.tracking_errors))


if __name__ == "__main__":
    unittest.main()
