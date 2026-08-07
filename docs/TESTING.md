# Testing strategy / 测试策略

The release gate combines several layers rather than treating one smoke test as
proof of correctness:

- native C API and model-manifest unit tests;
- real PP-OCRv6 model inference through the C example;
- golden HTTP regression with full OCR, blank input, cropped recognition,
  orientation classification, JSON/Base64, binary upload, and ordered batches;
- contract tests against the frozen OpenAPI and JSON schemas;
- malformed and hostile HTTP input tests followed by a recovery request;
- bounded engine-queue tests covering `429`, `503`, shutdown wake-up, and
  post-overload recovery;
- concurrency, varied image sizes, RSS-growth monitoring, and a 5,000-iteration
  scheduled Windows stability run;
- C ABI export checks and platform dependency checks;
- a Linux ASan/UBSan build covering native, HTTP, invalid-input, and stress
  paths.

发布门禁由多层测试组成：原生单元测试、真实模型推理、黄金正确性回归、冻结契约、
异常输入后恢复、并发与 RSS 增长、C ABI/平台依赖检查，以及 Linux ASan/UBSan。
Windows 定时任务额外执行 5,000 次长时间稳定性测试。

Run the repository-only checks without compiling:

```bash
python3 scripts/validate_model_manifest.py \
  --manifest models/ppocrv6-tiny/model.json --require-sha256
python3 scripts/validate_release_version.py
python3 scripts/validate_contracts.py
python3 scripts/validate_dependencies.py
python3 scripts/generate_sbom.py --version 1.1.0 \
  --output build/lw.PPOCR.OpenCVDNN.cdx.json
```

After installing a local package, run the live tests with distinct free ports:

```bash
python3 scripts/http_smoke.py --package-dir dist/package --port 18787
python3 scripts/config_contract_test.py --package-dir dist/package
python3 scripts/ocr_regression.py --package-dir dist/package \
  --cases tests/regression/ppocrv6-tiny.json --port 18786
python3 scripts/http_invalid_inputs.py --package-dir dist/package --port 18784
python3 scripts/http_contract_test.py --package-dir dist/package --port 18782
python3 scripts/http_benchmark.py --package-dir dist/package \
  --output build/http-benchmark.json --warmup 10 --iterations 100 \
  --matrix 1:1,1:4,2:2,2:4,4:4
python3 scripts/http_stress.py --package-dir dist/package \
  --iterations 5000 --concurrency 2 --port 18788 \
  --output build/http-stress.json
```

RSS growth is a practical leak signal, not a mathematical proof that no leak
exists. Allocator caches and model initialization can retain memory. ASan,
repeatable growth thresholds, recovery checks, and long runs are therefore used
together.

RSS 增长是内存泄漏的实用信号，但不是“绝对无泄漏”的数学证明；分配器缓存和模型
初始化也可能保留内存。因此项目把 ASan、可重复增长阈值、异常后恢复和长时间运行
结合使用。
