"""Self-made INT8 variants for the OCR benchmark (no official INT8 PaddleOCR ONNX exists in RapidAI's model list).

Dynamic quantization with onnxruntime.quantization (weights INT8, activations quantized at run time, no
calibration data). Labelled "self-quantized" in the report. Run in a venv that has onnxruntime + onnx:

    python -m venv ~/.venvs/ocrbench && ~/.venvs/ocrbench/Scripts/pip install --only-binary :all: onnxruntime onnx
    ~/.venvs/ocrbench/Scripts/python tools/ocr_bench/quantize.py <in.onnx> <out.onnx> [--matmul-only]
"""

import argparse
import os
import tempfile

import onnx
from onnx import numpy_helper
from onnxruntime.quantization import QuantType, quantize_dynamic


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--matmul-only", action="store_true",
                    help="quantize only MatMul (SVTR attention); ConvInteger is often slower than FP32 Conv on CPU")
    args = ap.parse_args()
    ops = ["MatMul"] if args.matmul_only else None
    # paddle2onnx stores some weights as Constant nodes; the quantizer only touches initializers.
    model = onnx.load(args.src)
    for node in list(model.graph.node):
        if node.op_type == "Constant" and node.attribute and node.attribute[0].name == "value":
            tensor = numpy_helper.from_array(numpy_helper.to_array(node.attribute[0].t), node.output[0])
            model.graph.initializer.append(tensor)
            model.graph.node.remove(node)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src.onnx")
        onnx.save(model, src)
        quantize_dynamic(src, args.dst, weight_type=QuantType.QInt8, op_types_to_quantize=ops)
    print(f"{args.src} -> {args.dst}")


if __name__ == "__main__":
    main()
