#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.host_viewer.config import ViewerConfig
from tools.host_viewer.controller import BoardController, EventLog


def _letterbox(src_w: int, src_h: int, model_w: int, model_h: int) -> tuple[float, int, int, int, int]:
    scale = min(model_w / src_w, model_h / src_h)
    resized_w = round(src_w * scale)
    resized_h = round(src_h * scale)
    pad_x = (model_w - resized_w) // 2
    pad_y = (model_h - resized_h) // 2
    return scale, resized_w, resized_h, pad_x, pad_y


def _map_x(model_x: float, scale: float, pad_x: int) -> int:
    return max(0, min(640, int((model_x - pad_x) / scale)))


def _map_y(model_y: float, scale: float, pad_y: int) -> int:
    return max(0, min(480, int((model_y - pad_y) / scale)))


def main() -> int:
    scale, resized_w, resized_h, pad_x, pad_y = _letterbox(640, 480, 640, 640)
    assert scale == 1.0
    assert (resized_w, resized_h) == (640, 480)
    assert (pad_x, pad_y) == (0, 80)
    assert _map_x(320, scale, pad_x) == 320
    assert _map_y(80, scale, pad_y) == 0
    assert _map_y(560, scale, pad_y) == 480

    assert ViewerConfig().preprocess_mode == "letterbox"
    cfg = ViewerConfig()
    command = BoardController(cfg, EventLog()).build_remote_stream_command()
    assert "--preprocess-mode letterbox" in command
    print("V3.2.2 letterbox validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
