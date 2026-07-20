#!/usr/bin/env python3
import argparse
from pathlib import Path
import types

import numpy as np
import onnx

if not hasattr(onnx, "mapping"):
    from onnx import _mapping

    tensor_type_to_np_type = {
        tensor_type: dtype_map.np_dtype
        for tensor_type, dtype_map in _mapping.TENSOR_TYPE_MAP.items()
    }
    np_type_to_tensor_type = {}
    for tensor_type, np_type in tensor_type_to_np_type.items():
        np_type_to_tensor_type[np_type] = tensor_type
        np_type_to_tensor_type[np.dtype(np_type)] = tensor_type
        np_type_to_tensor_type[np.dtype(np_type).type] = tensor_type

    onnx.mapping = types.SimpleNamespace(
        TENSOR_TYPE_TO_NP_TYPE=tensor_type_to_np_type,
        NP_TYPE_TO_TENSOR_TYPE=np_type_to_tensor_type,
    )

from rknn.api import RKNN


SCRIPT_DIR = Path(__file__).resolve().parent


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert optimized YOLOv6n ONNX to RK3568 RKNN."
    )
    parser.add_argument(
        "--onnx",
        default=str(SCRIPT_DIR / "yolov6n.onnx"),
        help="Input YOLOv6n ONNX model path.",
    )
    parser.add_argument(
        "--dataset",
        default=str(SCRIPT_DIR / "dataset.txt"),
        help="Quantization dataset list path.",
    )
    parser.add_argument(
        "--output",
        default=str(SCRIPT_DIR / "yolov6n-rk3568.rknn"),
        help="Output RKNN model path.",
    )
    parser.add_argument(
        "--target-platform",
        default="rk3568",
        help="RKNN target platform.",
    )
    parser.add_argument(
        "--no-quant",
        action="store_true",
        help="Build a non-quantized RKNN model for debugging only.",
    )
    return parser.parse_args()


def require_file(path, description):
    if not path.is_file():
        raise FileNotFoundError(f"{description} not found: {path}")


def main():
    args = parse_args()
    onnx_path = Path(args.onnx).resolve()
    dataset_path = Path(args.dataset).resolve()
    output_path = Path(args.output).resolve()
    do_quantization = not args.no_quant

    require_file(onnx_path, "ONNX model")
    if do_quantization:
        require_file(dataset_path, "quantization dataset")

    print("YOLOv6n RKNN conversion")
    print(f"onnx = {onnx_path}")
    print(f"dataset = {dataset_path if do_quantization else 'disabled'}")
    print(f"output = {output_path}")
    print(f"target_platform = {args.target_platform}")
    print(f"do_quantization = {do_quantization}")

    rknn = RKNN(verbose=True)
    try:
        ret = rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform=args.target_platform,
        )
        if ret != 0:
            raise RuntimeError(f"rknn.config failed: {ret}")

        print("--> Loading ONNX")
        ret = rknn.load_onnx(model=str(onnx_path))
        if ret != 0:
            raise RuntimeError(f"rknn.load_onnx failed: {ret}")
        print("done")

        print("--> Building RKNN")
        ret = rknn.build(
            do_quantization=do_quantization,
            dataset=str(dataset_path) if do_quantization else None,
        )
        if ret != 0:
            raise RuntimeError(f"rknn.build failed: {ret}")
        print("done")

        output_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"--> Export RKNN: {output_path}")
        ret = rknn.export_rknn(str(output_path))
        if ret != 0:
            raise RuntimeError(f"rknn.export_rknn failed: {ret}")
        print("done")
    finally:
        rknn.release()


if __name__ == "__main__":
    main()
