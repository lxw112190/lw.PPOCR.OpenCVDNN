# lw.PPOCR.OpenCVDNN

[中文](README.md)

A small, cross-platform PP-OCR inference project powered exclusively by **OpenCV DNN**. It exposes full OCR and cropped-text recognition through a compact C ABI, with C/C#/Python examples, a cpp-httplib HTTP service, a browser test page, and optional spdlog logging.

The bundled model is PP-OCRv6 Tiny Chinese and inference currently targets the CPU. Paddle Runtime, ONNX Runtime, DirectML, OpenVINO, and TensorRT are not required.

> Current stable release: `v1.0.0`. C ABI v1, HTTP API v1,
> configuration Schema v1, JSONL log Schema v1, and model-manifest Schema v1
> are formally frozen. The 1.x line preserves backward compatibility;
> breaking changes require a new major version or a versioned v2 contract.
> Validate the release package on the target environment before production use.

## Features

- Full OCR: detection, optional direction classification, and recognition.
- Recognition only: skips detection for pre-cropped text-line images. HTTP batches default to 32 images, may be configured up to 256, and are inferred in chunks of eight.
- C API: accepts encoded JPEG/PNG/BMP image bytes and returns UTF-8 JSON.
- C, C# P/Invoke, and Python ctypes examples.
- HTTP endpoints support direct binary image uploads, JSON/Base64 requests, and raw PDF uploads through `/api/ocr`, `/api/recognize`, `/api/pdf/ocr`, and `/health`; PDF pages select text-layer, OCR, or hybrid results.
- Browser page with detected regions, confidence scores, and stage timings.
- Optional API Key; runtime and request logging can be controlled independently.
- Production safeguards: bounded HTTP/engine queues, `429` on a full engine wait queue, `503` on engine wait timeout, and cumulative decoded-memory limits for batches.
- Nginx-inspired log operations: split runtime/access logs, JSON Lines, request IDs, stage timings, trusted proxies, and privacy safeguards.
- Docker and Docker Compose with a non-root Linux x64 image, health checks, persistent logs, and GHCR publishing.
- Correctness and robustness gates for fixed images, text order, orientation, confidence, boxes, batch order, malformed-input recovery, ASan/UBSan, and long-run memory growth.
- Automated compatibility checks for C ABI v1, model-manifest Schema v1, HTTP API v1, configuration Schema v1, and JSONL log Schema v1.
- Supply-chain controls with dependency/model hashes, a CycloneDX 1.6 SBOM, CodeQL, pull-request dependency review, and scheduled verification. Automated Dependabot update PRs are disabled; dependency upgrades are reviewed manually.
- Windows x64, Linux x64, Linux ARM64 (UnionTech UOS 20 compatibility baseline), and macOS ARM64 CI, with platform-neutral inference code.

## Performance snapshot

The following results were measured locally on 2026-08-01 with an AMD Ryzen 7
7735H (8 cores / 16 threads), 16 GB RAM, Windows 10 22H2, and OpenCV DNN 5.0.0
on CPU. The HTTP client used loopback, keep-alive, and binary image bodies. Each
scenario was warmed up concurrently before measurement, with file logging
disabled during the benchmark.

| Workload | Engines / concurrency | Throughput | Client P95 | Warm RSS |
| --- | ---: | ---: | ---: | ---: |
| Full OCR | 1 / 1 | 6.17 req/s | 177.97 ms | about 151 MB |
| Full OCR | 2 / 2 | 6.98 req/s | 336.50 ms | about 284 MB |
| Recognition only (cropped line) | 1 / 1 | 126.14 req/s | 9.39 ms | about 154 MB |
| Recognition only (cropped line) | 2 / 2 | 172.69 req/s | 13.68 ms | about 287 MB |
| Recognition only (cropped line) | 4 / 4 | 199.80 req/s | 24.32 ms | about 551 MB |

All 1,000 varied-size requests in the concurrency-4 stability run succeeded.
RSS grew by `2.32 MB` net, and normal OCR recovered after malformed-input tests.
Four engines reduced full-OCR throughput on this machine because of CPU
contention, so start with `engine_instances=1` for full OCR. Two engines are a
reasonable first benchmark for cropped-line recognition. These measurements
are sizing guidance, not a performance guarantee for other images, CPUs, or
operating systems.

See the [v1.0.0-rc.3 Windows local performance report](docs/performance/v1.0.0-rc.3-windows-local.md)
for the complete environment, methodology, latency percentiles, memory data,
and reproduction commands.

## HTTP quick start

Run the executable from the extracted package directory:

```powershell
# Windows
.\lw-ppocr-http-service.exe
```

```bash
# Linux
chmod +x lw-ppocr-http-service
./lw-ppocr-http-service
```

For UnionTech UOS 20 ARM64, download the artifact containing
`linux-arm64-uos20`. It is built natively against a Debian 10 ARM64 glibc 2.28
and GCC 8.3 baseline. See [Linux ARM64 and UnionTech UOS 20](docs/LINUX-ARM64-UOS20.md)
for deployment steps and the precise compatibility statement.

```bash
# macOS Apple Silicon
chmod +x run-http-service.sh
./run-http-service.sh
```

Open <http://127.0.0.1:8787/>. Startup output always shows the author, QQ contact, configuration path, and all effective parameters, even when spdlog is disabled. The QQ group is documented here only and is not printed by the service or web page.

### Docker / Docker Compose

`v1.0.0` provides the stable `linux/amd64` image. After the tag is published:

```bash
docker run -d --name lw-ppocr --restart unless-stopped \
  -p 8787:8787 -v lw-ppocr-logs:/data/logs \
  ghcr.io/lxw112190/lw.ppocr.opencvdnn:1.0.0
```

With Compose:

```bash
cp .env.example .env
docker compose up -d
docker compose ps
```

Set `PPOCR_API_KEY` in `.env` to enable authentication. The image runs as a non-root user and includes OpenCV, models, web assets, a health check, and persistent rotating logs. See [docs/DOCKER.md](docs/DOCKER.md) for all options.

### Install as a system service

On Windows, edit `http-service.json`, then run these scripts as Administrator:

```bat
install-service.bat
stop-service.bat
restart-service.bat
start-service.bat
uninstall-service.bat
```

The service name is `lw.PPOCR.OpenCVDNN`. It starts automatically and restarts after failures. The installer records the package's absolute path, so uninstall the service before moving or deleting the extracted directory, then reinstall it from the new location.

On a systemd-based Linux distribution, run from the package directory:

```bash
sudo ./install-service.sh
sudo ./stop-service.sh
sudo ./restart-service.sh
sudo ./start-service.sh
sudo ./uninstall-service.sh
```

The service name is `lw-ppocr-opencvdnn.service`. By default it runs as the user who invoked `sudo` and starts at boot. Override the account during installation with `sudo LW_PPOCR_SERVICE_USER=ocr ./install-service.sh`. Logs remain under the package's `logs/` directory. The installer applies systemd path escaping to the working directory for compatibility with openEuler 22.03 SP1/systemd 249. Run `./install-service.sh --verify-only` first to have `systemd-analyze` validate the unit without changing the system. If installation or startup fails, the script prints the unit, service status, and the latest 80 journal records automatically.

For full OCR, upload the encoded image bytes directly to avoid Base64's roughly
33% size increase:

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: image/jpeg" \
  --data-binary @image.jpg
```

The JSON/Base64 form remains compatible:

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: application/json" \
  -d '{"image_base64":"..."}'
```

Cropped-text recognition also accepts binary data:

```bash
curl http://127.0.0.1:8787/api/recognize \
  -H "Content-Type: image/png" \
  --data-binary @cropped-text.png
```

Batch recognition remains JSON/Base64:

```json
{"images_base64":["...","..."]}
```

Binary single-image requests accept `image/*` or `application/octet-stream`.
Both raw Base64 and `data:image/png;base64,...` URLs are accepted in JSON. The
browser page uses binary upload by default.

See [docs/HTTP-API.md](docs/HTTP-API.md) for request, response, and status-code details. JSON responses include `X-LW-PPOCR-API-Version: 1`; clients should branch on stable `error_code` values rather than parsing the diagnostic `error` text.

### API Key

API Key authentication is disabled by default. Set a non-empty `api_key` in [http-service.json](http-service.json) and restart the service. Clients must then send the secret in the `X-API-Key` header. The browser page includes a matching input field.

The service never logs API Keys, Base64 image data, request bodies, or recognized text. For production, also use HTTPS through a reverse proxy and configure appropriate network and request-size restrictions.

### Logging

v0.4.0 separates human-readable runtime diagnostics in `logs/runtime.log` from OCR access records in `logs/access.log`. Access logging defaults to `jsonl`, matching the frozen machine-readable access-log Schema v1; set `access_format` to `text` when the timestamped human-readable style is preferred. Access records include the request ID, status, input/output sizes, image dimensions, stage timings, and stable error codes. Every OCR response returns the same ID in its JSON body and `X-Request-ID` header.

Set `enabled=false` to disable spdlog entirely, or `request_enabled=false` to keep runtime logs while disabling access records and request-start breadcrumbs. With `request_start_enabled=true`, the service explicitly flushes a `request_started` record before inference; access records are periodically flushed according to `flush_interval_seconds`. `X-Forwarded-For` is ignored unless the immediate proxy IP is explicitly listed in `trusted_proxies`. Caught failures, `std::terminate`, and Windows unhandled-exception code/address are recorded on a best-effort basis.

Logs are not crash dumps and cannot guarantee a final write after power loss, `kill -9`, or severe memory corruption. See [docs/LOGGING.md](docs/LOGGING.md) for recommended Windows dump and Linux core-dump setup.

## C API

The complete public surface is in [include/lw/ppocr.h](include/lw/ppocr.h). The normal lifecycle is:

1. `lw_ppocr_config_init`
2. Set `model_manifest_utf8`
3. `lw_ppocr_create`
4. Call `lw_ppocr_ocr_encoded` or `lw_ppocr_recognize_encoded`
5. Release JSON with `lw_ppocr_string_free`
6. `lw_ppocr_destroy`

See [examples/c/main.c](examples/c/main.c), [examples/csharp](examples/csharp), and [examples/python/ocr.py](examples/python/ocr.py). See [docs/C-ABI.md](docs/C-ABI.md) for the C ABI v1 compatibility rules, [docs/MODEL-MANIFEST.md](docs/MODEL-MANIFEST.md) for model Schema v1, and [docs/CONTRACTS.md](docs/CONTRACTS.md) for the frozen HTTP/configuration/log contracts.

The C# example targets .NET 8 and calls the same native C ABI on Windows or Linux:

```powershell
dotnet run --project examples/csharp -c Release -- `
  dist/local-win-x64 `
  models/ppocrv6-tiny/model.json `
  models/ppocrv6-tiny/sample.jpg `
  ocr
```

## Build from source

Requirements: CMake 3.24+, a C++17 compiler, and OpenCV 5.0+ with `core`, `imgproc`, `imgcodecs`, `dnn`, and their dependency modules.

The project supports OpenCV 5 only and is not compatible with OpenCV 4.x. OpenCV 5 requires C++17, and the project also uses C++17 facilities such as `std::filesystem`. Consumers call a stable C ABI and do not need to use C++17 themselves. GCC 8 builds automatically link `stdc++fs` for UOS 20 / Debian 10-era toolchains. Maintain a separate compatibility project for OpenCV 4.x or legacy systems limited to a C++11 toolchain.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR=/path/to/opencv
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/package
```

CI artifacts are assembled for immediate use after extraction. They include the OCR runtime, OpenCV, models, configuration, web assets, and examples. Windows also bundles the VC143 x64 runtime. Linux bundles `libstdc++.so.6` and `libgcc_s.so.1`, while image-codec dependencies are linked into OpenCV statically. glibc, the Linux ELF loader, Windows system DLLs, and macOS system frameworks remain platform dependencies, so the target still needs to match the documented OS and architecture baseline.

Linux and macOS CI save OpenCV only after a successful build and install, while Windows CI caches the extracted official prebuilt package. All four native workflows verify model SHA-256 values, frozen contracts, the dependency lock, C ABI exports, golden OCR correctness, unit tests, HTTP behavior, varied-image concurrency, malformed requests, and RSS growth; Linux also runs ASan/UBSan. The Linux ARM64 workflow additionally verifies AArch64 ELF files and the maximum required GLIBC symbol version. The Windows workflow runs 5,000 stability iterations nightly and 64 iterations on regular changes. Docker CI validates non-root execution, Compose, health checks, API Key enforcement, and binary OCR. Formal `v*` tags publish the GHCR image. Release attachments include SHA-256 checksums, and packages include the dependency lock, contract schemas, and CycloneDX SBOM.

## Quality, security, and compatibility documents

- [v1.0.0 stable release notes](docs/releases/v1.0.0.md)
- [v1.0.0-rc.3 release candidate notes](docs/releases/v1.0.0-rc.3.md)
- [v1.0.0-rc.2 release candidate notes](docs/releases/v1.0.0-rc.2.md)
- [Compatibility matrix](docs/COMPATIBILITY.md)
- [Frozen v1 contracts](docs/CONTRACTS.md)
- [Testing strategy and local commands](docs/TESTING.md)
- [v1.0.0-rc.3 Windows local performance report](docs/performance/v1.0.0-rc.3-windows-local.md)
- [Dependency lock, SBOM, and supply-chain controls](docs/SUPPLY-CHAIN.md)
- [Security reporting policy](SECURITY.md)

## Concurrency

One engine instance serializes inference. The HTTP service uses `engine_instances` independent instances for parallel work; every additional instance loads another copy of all three models. Start with `engine_instances=1`, `worker_threads=4`, `max_queued_requests=32`, and `engine_wait_timeout_ms=5000`, then tune on the target machine. The HTTP pool keeps `worker_threads` base threads and may temporarily expand by the bounded wait allowance during overload. Requests that reach application admission receive `429` when the engine wait queue is full and `503` when the wait times out. The low-level task queue uses the same hard bound and may reject the connection during a more extreme burst.

## License and contact

Project code is MIT licensed. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and `licenses/` for bundled dependencies and model terms.

- Author: 天天代码码天天
- QQ: 819069052
- QQ Group: C# 人工智能实践 | 758616458
- Project: <https://github.com/lxw112190/lw.PPOCR.OpenCVDNN>

<img src="docs/assets/sponsor.jpg" alt="Sponsor QR code" width="240">
