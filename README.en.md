# lw.PPOCR.OpenCVDNN

[中文](README.md)

A small, cross-platform PP-OCR inference project powered exclusively by **OpenCV DNN**. It exposes full OCR and cropped-text recognition through a compact C ABI, with C/C#/Python examples, a cpp-httplib HTTP service, a browser test page, and optional spdlog logging.

The bundled model is PP-OCRv6 Tiny Chinese and inference currently targets the CPU. Paddle Runtime, ONNX Runtime, DirectML, OpenVINO, and TensorRT are not required.

> Current version: `v0.1.0`. The public API is being validated and is not yet covered by a long-term ABI freeze commitment.

## Features

- Full OCR: detection, optional direction classification, and recognition.
- Recognition only: skips detection for pre-cropped text-line images; batches of 1–256 images are supported.
- C API: accepts encoded JPEG/PNG/BMP image bytes and returns UTF-8 JSON.
- C, C# P/Invoke, and Python ctypes examples.
- HTTP endpoints: `/api/ocr`, `/api/recognize`, and `/health`.
- Browser page with detected regions, confidence scores, and stage timings.
- Optional API Key; runtime and request logging can be controlled independently.
- Windows x64 and Linux x64 CI, with platform-neutral inference code.

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

Open <http://127.0.0.1:8787/>. Startup output always shows the author, QQ contact, configuration path, and all effective parameters, even when spdlog is disabled. The QQ group is documented here only and is not printed by the service or web page.

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

The service name is `lw-ppocr-opencvdnn.service`. By default it runs as the user who invoked `sudo` and starts at boot. Override the account during installation with `sudo LW_PPOCR_SERVICE_USER=ocr ./install-service.sh`. Logs remain under the package's `logs/` directory.

Full OCR:

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: application/json" \
  -d '{"image_base64":"..."}'
```

Cropped-text recognition uses `/api/recognize`. Send either `image_base64` or a batch:

```json
{"images_base64":["...","..."]}
```

Both raw Base64 and `data:image/png;base64,...` URLs are accepted.

See [docs/HTTP-API.md](docs/HTTP-API.md) for request, response, and status-code details.

### API Key

API Key authentication is disabled by default. Set a non-empty `api_key` in [http-service.json](http-service.json) and restart the service. Clients must then send the secret in the `X-API-Key` header. The browser page includes a matching input field.

The service never logs API Keys, Base64 image data, request bodies, or recognized text. For production, also use HTTPS through a reverse proxy and configure appropriate network and request-size restrictions.

### Logging

The `logging` object in `http-service.json` controls console and rotating-file logs. Set `enabled=false` to disable spdlog entirely, or `request_enabled=false` to keep runtime logs while disabling per-request records. Each request writes and flushes a `request_started` breadcrumb before inference, followed by status, timing, and result count. Caught failures, `std::terminate`, and Windows unhandled-exception code/address are recorded on a best-effort basis.

Logs are not crash dumps and cannot guarantee a final write after power loss, `kill -9`, or severe memory corruption. See [docs/LOGGING.md](docs/LOGGING.md) for recommended Windows dump and Linux core-dump setup.

## C API

The complete public surface is in [include/lw/ppocr.h](include/lw/ppocr.h). The normal lifecycle is:

1. `lw_ppocr_config_init`
2. Set `model_manifest_utf8`
3. `lw_ppocr_create`
4. Call `lw_ppocr_ocr_encoded` or `lw_ppocr_recognize_encoded`
5. Release JSON with `lw_ppocr_string_free`
6. `lw_ppocr_destroy`

See [examples/c/main.c](examples/c/main.c), [examples/csharp](examples/csharp), and [examples/python/ocr.py](examples/python/ocr.py).

The C# example targets .NET 8 and calls the same native C ABI on Windows or Linux:

```powershell
dotnet run --project examples/csharp -c Release -- `
  dist/local-win-x64 `
  models/ppocrv6-tiny/model.json `
  models/ppocrv6-tiny/sample.jpg `
  ocr
```

## Build from source

Requirements: CMake 3.24+, a C++17 compiler, and OpenCV 4.5+ with `core`, `imgproc`, `imgcodecs`, and `dnn`.

The project intentionally stays on C++17: OpenCV 5, used by the official CI builds, requires C++17, and the project also uses C++17 facilities such as `std::filesystem`. Consumers call a stable C ABI and do not need to use C++17 themselves. GCC 8 builds automatically link `stdc++fs` for UOS 20 / Debian 10-era toolchains. For legacy systems limited to a C++11 toolchain, maintain a separate OpenCV 4.x + C++11 compatibility branch.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR=/path/to/opencv
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/package
```

CI artifacts are assembled for immediate use after extraction. They include the OCR runtime, OpenCV, models, configuration, web assets, and examples. Windows also bundles the VC143 x64 runtime. Linux bundles `libstdc++.so.6` and `libgcc_s.so.1`, while image-codec dependencies are linked into OpenCV statically. glibc, the Linux ELF loader, and Windows system DLLs remain platform dependencies, so the target still needs to match the documented OS and architecture baseline.

Linux CI saves OpenCV only after a successful build and install, while Windows CI caches the extracted official prebuilt package. Both workflows upload the package archive and its SHA-256 checksum.

## Concurrency

One engine instance serializes inference. The HTTP service uses `engine_instances` independent instances for parallel work; every additional instance loads another copy of all three models. Start with one engine and four HTTP workers, then tune using measurements from the target machine.

## License and contact

Project code is MIT licensed. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and `licenses/` for bundled dependencies and model terms.

- Author: 天天代码码天天
- QQ: 819069052
- QQ Group: C# 人工智能实践 | 758616458
- Project: <https://github.com/lxw112190/lw.PPOCR.OpenCVDNN>

<img src="docs/assets/sponsor.jpg" alt="Sponsor QR code" width="240">
