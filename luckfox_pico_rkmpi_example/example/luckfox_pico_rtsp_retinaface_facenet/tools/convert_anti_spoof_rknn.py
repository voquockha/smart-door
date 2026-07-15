#!/usr/bin/env python3
"""Convert MiniFASNetV2 ONNX to an RV1106-compatible RKNN model."""

import argparse
from pathlib import Path

from rknn.api import RKNN


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--onnx", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--dataset",
        required=True,
        help="Text file containing representative 80x80 BGR image paths",
    )
    return parser.parse_args()


def check(ret: int, step: str) -> None:
    if ret != 0:
        raise RuntimeError(f"RKNN {step} failed: {ret}")


def main() -> int:
    args = parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=True)
    try:
        # This official checkpoint was trained with the repository's modified
        # ToTensor(), which keeps BGR byte values in [0, 255].
        check(
            rknn.config(
                target_platform="rv1106",
                mean_values=[[0.0, 0.0, 0.0]],
                std_values=[[1.0, 1.0, 1.0]],
                quantized_dtype="asymmetric_quantized-8",
                quantized_algorithm="normal",
                optimization_level=3,
            ),
            "config",
        )
        check(rknn.load_onnx(model=args.onnx), "load_onnx")
        check(
            rknn.build(
                do_quantization=True,
                dataset=args.dataset,
            ),
            "build",
        )
        check(rknn.export_rknn(str(output)), "export_rknn")
    finally:
        rknn.release()

    print(f"RKNN exported: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
