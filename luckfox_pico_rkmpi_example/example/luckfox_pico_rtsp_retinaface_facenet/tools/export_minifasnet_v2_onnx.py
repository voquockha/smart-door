#!/usr/bin/env python3
"""Export the official MiniFASNetV2 checkpoint to a fixed-shape ONNX model."""

import argparse
from collections import OrderedDict
from pathlib import Path
import sys

import onnx
import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-repo",
        required=True,
        help="Path to minivision-ai/Silent-Face-Anti-Spoofing",
    )
    parser.add_argument("--weights", required=True, help="MiniFASNetV2 .pth file")
    parser.add_argument("--output", required=True, help="Output .onnx file")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_repo = Path(args.source_repo).resolve()
    sys.path.insert(0, str(source_repo))

    from src.model_lib.MiniFASNet import MiniFASNetV2  # pylint: disable=import-error
    from src.utility import get_kernel  # pylint: disable=import-error

    model = MiniFASNetV2(conv6_kernel=get_kernel(80, 80))
    state = torch.load(args.weights, map_location="cpu", weights_only=False)
    if next(iter(state)).startswith("module."):
        state = OrderedDict((key[7:], value) for key, value in state.items())
    model.load_state_dict(state)
    model.eval()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(1, 3, 80, 80, dtype=torch.float32)
    with torch.no_grad():
        torch.onnx.export(
            model,
            dummy,
            str(output),
            input_names=["input"],
            output_names=["logits"],
            opset_version=12,
            do_constant_folding=True,
            dynamic_axes=None,
            dynamo=False,
        )

    graph = onnx.load(str(output))
    onnx.checker.check_model(graph)
    print(f"ONNX exported: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
