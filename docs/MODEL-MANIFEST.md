# Model manifest Schema v1 / 模型清单 Schema v1

The model manifest format is frozen at `schema_version: 1` starting with
v0.3.0. Its machine-readable definition is
`models/model-manifest.schema.json`.

从 v0.3.0 开始，模型清单格式冻结为 `schema_version: 1`，机器可读定义位于
`models/model-manifest.schema.json`。

## Required structure / 必需结构

- Root: `schema_version`, `name`, `family`, `dictionary`, and `stages`.
- `stages.detector` and `stages.recognizer` are required;
  `stages.classifier` is optional.
- Every stage contains `input_shape` and one `artifacts.onnx` entry.
- An ONNX artifact contains `path`, `format: onnx`, and `precision`, where
  precision is `fp32`, `fp16`, or `int8`.
- `sha256` is optional for third-party manifests. The model shipped by this
  repository always includes and verifies SHA-256 for the dictionary and every
  ONNX file.
- Unknown properties are rejected. New optional properties therefore require a
  new documented schema version instead of being silently ignored.

- 根节点必须包含 `schema_version`、`name`、`family`、`dictionary` 和 `stages`。
- `detector`、`recognizer` 必需，`classifier` 可选。
- 每个阶段必须包含 `input_shape` 和唯一的 `artifacts.onnx`。
- ONNX 产物必须声明 `path`、`format: onnx` 和 `precision`；精度只能是
  `fp32`、`fp16` 或 `int8`。
- 第三方清单可以不写 `sha256`；仓库内置模型必须为字典及全部 ONNX 文件提供并验证
  SHA-256。
- 未定义字段会被拒绝，防止拼写错误或未来字段被旧运行时静默忽略。

Validate a manifest and all declared file hashes:

```bash
python scripts/validate_model_manifest.py \
  --manifest models/ppocrv6-tiny/model.json \
  --require-sha256
```

Runtime loading performs the same structural validation before loading OpenCV
DNN networks. CI additionally verifies file hashes and runs malformed-manifest
unit cases.

运行时会在 OpenCV DNN 加载模型前执行严格结构校验；CI 还会核对文件哈希，并验证
未知字段、缺失字段、错误版本、错误格式和非法哈希均能被拒绝。
