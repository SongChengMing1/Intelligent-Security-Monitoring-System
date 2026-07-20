#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


REQUIRED_SEQUENCES = {
    "single_target_motion",
    "multiple_targets",
    "crossing_targets",
    "short_miss",
    "low_confidence_recovery",
    "exit_reentry",
    "class_mismatch",
    "timestamp_gap",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate V3.5.0 deterministic tracking sequences")
    parser.add_argument(
        "path",
        nargs="?",
        type=Path,
        default=Path("tests/data/v350_tracking_sequences.csv"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    frames: dict[str, dict[int, int]] = defaultdict(dict)
    last_timestamp: dict[str, int] = {}
    observations = 0
    low_confidence = 0
    empty_frames = 0

    with args.path.open(newline="", encoding="utf-8") as stream:
        rows = csv.DictReader(stream)
        required_columns = {
            "sequence",
            "frame_id",
            "timestamp_ms",
            "object_key",
            "class_id",
            "class_name",
            "score",
            "x1",
            "y1",
            "x2",
            "y2",
        }
        if set(rows.fieldnames or ()) != required_columns:
            raise ValueError(f"unexpected columns: {rows.fieldnames}")

        for line_number, row in enumerate(rows, start=2):
            sequence = row["sequence"]
            frame_id = int(row["frame_id"])
            timestamp_ms = int(row["timestamp_ms"])
            score = float(row["score"])
            bbox = tuple(int(row[key]) for key in ("x1", "y1", "x2", "y2"))

            frame_timestamp = frames[sequence].setdefault(frame_id, timestamp_ms)
            if frame_timestamp != timestamp_ms:
                raise ValueError(f"line {line_number}: one frame has multiple timestamps")
            previous = last_timestamp.get(sequence)
            if previous is not None and timestamp_ms < previous:
                raise ValueError(f"line {line_number}: timestamp moved backwards")
            last_timestamp[sequence] = timestamp_ms

            if row["object_key"] == "-":
                if score != 0.0 or bbox != (0, 0, 0, 0):
                    raise ValueError(f"line {line_number}: empty frame has object data")
                empty_frames += 1
                continue

            if not 0.0 < score <= 1.0:
                raise ValueError(f"line {line_number}: score is outside (0, 1]")
            x1, y1, x2, y2 = bbox
            if x1 < 0 or y1 < 0 or x2 <= x1 or y2 <= y1 or x2 > 640 or y2 > 480:
                raise ValueError(f"line {line_number}: invalid 640x480 bbox {bbox}")
            observations += 1
            if score < 0.5:
                low_confidence += 1

    missing = REQUIRED_SEQUENCES - set(frames)
    if missing:
        raise ValueError(f"missing required sequences: {sorted(missing)}")
    if low_confidence == 0 or empty_frames == 0:
        raise ValueError("fixtures must include low-confidence observations and empty frames")

    unique_frames = sum(len(sequence_frames) for sequence_frames in frames.values())
    print(
        "valid "
        f"sequences={len(frames)} frames={unique_frames} observations={observations} "
        f"low_confidence={low_confidence} empty_frames={empty_frames}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
