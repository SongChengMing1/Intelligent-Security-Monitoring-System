import json
import unittest
from pathlib import Path

from tools.host_viewer.tracking_profile import (
    TrackingProfileError,
    canonical_profile_json,
    load_tracking_profile,
    profile_hash,
    effective_profile_mismatches,
    resolve_tracking_profile,
)


FIXTURES = Path(__file__).parent / "data"


class TrackingProfileContractTest(unittest.TestCase):
    def test_valid_source_matches_shared_golden(self) -> None:
        resolved = load_tracking_profile(FIXTURES / "v351_profile_source.json")
        golden = json.loads((FIXTURES / "v351_profile_golden.json").read_text(encoding="utf-8"))
        self.assertEqual(canonical_profile_json(resolved), golden["canonical_json"])
        self.assertEqual(profile_hash(resolved), golden["profile_hash"])
        self.assertEqual(resolved["observation"]["edge_margin"], 0.0)
        self.assertEqual(resolved["tracker"]["confirm_hits"], 2)

    def test_description_and_formatting_do_not_change_hash(self) -> None:
        source = json.loads((FIXTURES / "v351_profile_source.json").read_text(encoding="utf-8"))
        first = resolve_tracking_profile(source)
        source["description"] = "another display-only value"
        second = resolve_tracking_profile(source)
        self.assertEqual(profile_hash(first), profile_hash(second))

    def test_invalid_duplicate_classes_has_field_error(self) -> None:
        with self.assertRaisesRegex(TrackingProfileError, "allowed_class_ids must be unique"):
            load_tracking_profile(FIXTURES / "v351_profile_invalid.json")

    def test_unknown_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(TrackingProfileError, "unknown field"):
            resolve_tracking_profile({"profile_id": "bad", "runtime_hot_reload": True})

    def test_non_finite_number_is_rejected_by_canonical_json(self) -> None:
        resolved = resolve_tracking_profile({"profile_id": "finite"})
        resolved["detector"]["high_threshold"] = float("nan")
        with self.assertRaises(TrackingProfileError):
            canonical_profile_json(resolved)

    def test_effective_profile_mismatch_is_reported(self) -> None:
        resolved = resolve_tracking_profile({"profile_id": "indoor"})
        self.assertEqual([], effective_profile_mismatches({
            "profile_id": "indoor", "profile_hash": profile_hash(resolved)}, resolved))
        mismatches = effective_profile_mismatches({"profile_id": "other", "profile_hash": "bad"}, resolved)
        self.assertEqual(2, len(mismatches))


if __name__ == "__main__":
    unittest.main()
