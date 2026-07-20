import json
import tempfile
import unittest
from pathlib import Path

from tools.host_viewer.profile_catalog import (
    CameraRegion,
    CameraRegionError,
    ProfileCatalogError,
    apply_camera_region,
    discover_tracking_profiles,
    load_camera_region,
)
from tools.host_viewer.tracking_profile import load_tracking_profile, profile_hash


FIXTURES = Path(__file__).parent / "data"


class ProfileCatalogTest(unittest.TestCase):
    def test_repository_catalog_contains_initial_scene_set_and_camera0(self) -> None:
        root = Path(__file__).parents[1]
        presets = discover_tracking_profiles(root / "profiles" / "tracking")
        region = load_camera_region(root / "profiles" / "cameras" / "camera0.json")

        self.assertEqual(
            ["default-general", "indoor-person", "outdoor-person", "vehicle-monitoring"],
            [item.profile_id for item in presets],
        )
        self.assertEqual("camera0", region.camera_id)
        self.assertFalse(region.roi["enabled"])
        effective = apply_camera_region(presets[0].resolved, region)
        self.assertTrue(profile_hash(effective).startswith("sha256:"))

    def test_discovers_sorted_presets_with_description_and_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "zeta.json").write_text(
                json.dumps({
                    "profile_id": "zeta",
                    "description": "Zeta scene",
                }),
                encoding="utf-8",
            )
            (root / "alpha.json").write_text(
                json.dumps({
                    "profile_id": "alpha",
                    "description": "Alpha scene",
                }),
                encoding="utf-8",
            )

            presets = discover_tracking_profiles(root)

        self.assertEqual(["alpha", "zeta"], [item.profile_id for item in presets])
        self.assertEqual("Alpha scene", presets[0].description)
        self.assertTrue(presets[0].profile_hash.startswith("sha256:"))

    def test_rejects_duplicate_profile_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = {"profile_id": "same", "description": "duplicate"}
            (root / "first.json").write_text(json.dumps(source), encoding="utf-8")
            (root / "second.json").write_text(json.dumps(source), encoding="utf-8")

            with self.assertRaisesRegex(ProfileCatalogError, "duplicate profile_id"):
                discover_tracking_profiles(root)

    def test_camera_region_replaces_only_effective_roi_and_changes_hash(self) -> None:
        base = load_tracking_profile(FIXTURES / "v351_profile_source.json")
        base_hash = profile_hash(base)
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "camera0.json"
            path.write_text(
                json.dumps({
                    "camera_id": "camera0",
                    "description": "Main camera entrance",
                    "roi": {"enabled": True, "x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
                }),
                encoding="utf-8",
            )
            region = load_camera_region(path)
            effective = apply_camera_region(base, region)

        self.assertEqual("camera0", region.camera_id)
        self.assertTrue(effective["observation"]["roi"]["enabled"])
        self.assertEqual(0.1, effective["observation"]["roi"]["x1"])
        self.assertNotEqual(base_hash, profile_hash(effective))
        self.assertEqual(base["tracker"], effective["tracker"])

    def test_disabled_camera_region_uses_canonical_full_frame_roi(self) -> None:
        base = load_tracking_profile(FIXTURES / "v351_profile_source.json")
        region = CameraRegion(
            camera_id="camera0",
            description="Main camera entrance",
            roi={"enabled": False, "x1": 0.25, "y1": 0.25, "x2": 0.75, "y2": 0.75},
            source_path=Path("camera0.json"),
        )

        effective = apply_camera_region(base, region)

        self.assertEqual(
            {"enabled": False, "x1": 0.0, "y1": 0.0, "x2": 1.0, "y2": 1.0},
            effective["observation"]["roi"],
        )
        self.assertEqual(profile_hash(base), profile_hash(effective))

    def test_invalid_camera_region_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "camera0.json"
            path.write_text(
                json.dumps({
                    "camera_id": "camera0",
                    "description": "Broken camera",
                    "roi": {"enabled": True, "x1": 0.8, "y1": 0.2, "x2": 0.1, "y2": 0.9},
                }),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(CameraRegionError, "normalized ordered bounds"):
                load_camera_region(path)


if __name__ == "__main__":
    unittest.main()
