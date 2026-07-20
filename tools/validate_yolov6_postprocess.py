#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
POSTPROCESS_CC = PROJECT_ROOT / "src" / "postprocess.cc"


def _overlap(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> float:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    ax2 = ax + aw
    ay2 = ay + ah
    bx2 = bx + bw
    by2 = by + bh
    inter_w = max(0.0, min(ax2, bx2) - max(ax, bx) + 1.0)
    inter_h = max(0.0, min(ay2, by2) - max(ay, by) + 1.0)
    inter = inter_w * inter_h
    union = (aw + 1.0) * (ah + 1.0) + (bw + 1.0) * (bh + 1.0) - inter
    return 0.0 if union <= 0.0 else inter / union


def _nms(boxes: list[tuple[float, float, float, float]], class_ids: list[int],
         order: list[int], filter_id: int, threshold: float) -> list[int]:
    result = list(order)
    for i in range(len(result)):
        n = result[i]
        if n == -1 or class_ids[n] != filter_id:
            continue
        for j in range(i + 1, len(result)):
            m = result[j]
            if m == -1 or class_ids[m] != filter_id:
                continue
            if _overlap(boxes[n], boxes[m]) > threshold:
                result[j] = -1
    return result


def _assert_source_uses_sorted_candidate_indices() -> None:
    source = POSTPROCESS_CC.read_text(encoding="utf-8")
    required = [
        "const std::vector<int> &classIds",
        "int n = order[i];",
        "classIds[n] != filterId",
        "int m = order[j];",
        "classIds[m] != filterId",
        "sum/clip prefilter",
        "objProbs.push_back(maxClassProb)",
    ]
    for needle in required:
        assert needle in source, f"missing postprocess source pattern: {needle}"
    assert source.count("group->results[last_count].class_id = id;") == 1, (
        "YOLOv6 postprocess must preserve class_id"
    )


def main() -> int:
    _assert_source_uses_sorted_candidate_indices()

    boxes = [
        (100.0, 100.0, 50.0, 50.0),  # candidate 0, class 1
        (100.0, 100.0, 50.0, 50.0),  # candidate 1, class 2
        (102.0, 102.0, 50.0, 50.0),  # candidate 2, class 2, overlaps candidate 1
    ]
    class_ids = [1, 2, 2]
    sorted_order = [1, 0, 2]

    after_class_2 = _nms(boxes, class_ids, sorted_order, filter_id=2, threshold=0.5)
    assert after_class_2 == [1, 0, -1], after_class_2

    after_class_1 = _nms(boxes, class_ids, sorted_order, filter_id=1, threshold=0.5)
    assert after_class_1 == [1, 0, 2], after_class_1

    print("YOLOv6 postprocess validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
