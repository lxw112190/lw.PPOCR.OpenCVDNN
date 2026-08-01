# lw.PPOCR.OpenCVDNN

[English](README.en.md)

一个只使用 **OpenCV DNN** 的精简、跨平台 PP-OCR 推理项目。它把完整 OCR 与裁剪文字识别收敛到稳定的 C ABI，并同时提供 C/C#/Python 示例、基于 cpp-httplib 的 HTTP 服务、测试网页和可选的 spdlog 日志。

当前内置模型为 PP-OCRv6 Tiny Chinese，推理设备为 CPU。项目不依赖 Paddle Runtime、ONNX Runtime、DirectML、OpenVINO 或 TensorRT。

> 当前版本：`v0.4.0`。C ABI v1 与模型清单 Schema v1 保持冻结，本版重点升级生产日志与请求诊断。

## 主要能力

- 完整 OCR：检测 → 可选方向分类 → 文字识别。
- 仅识别：客户已裁剪文字区域时，跳过检测并支持 1～256 张批量识别。
- 稳定 C API：输入 JPEG/PNG/BMP 等编码图片字节，输出 UTF-8 JSON。
- C、C# P/Invoke 和 Python ctypes 调用示例。
- HTTP API：支持图片二进制直传和兼容的 JSON/Base64 请求，提供 `/api/ocr`、`/api/recognize`、`/health`；`result` 字段与 `lw.PPOCR.Inference` 保持一致。
- 浏览器体验页：显示识别文本、置信度、阶段耗时，并在原图绘制文字区域。
- 可选 API Key；运行日志和请求日志可分别开关。
- Nginx 风格日志治理：runtime/access 分离、JSON Lines、请求 ID、阶段耗时、可信代理与隐私保护。
- Docker / Docker Compose：Linux x64 非 root 镜像、健康检查、持久化日志和 GHCR 发布。
- 正确性回归：固定样图、文字顺序、方向分类、置信度与检测框容差均有黄金基线。
- 接口契约：C ABI v1 导出符号、结构布局与模型清单 Schema v1 自动防回退。
- Windows x64、Linux x64、Linux ARM64（统信 UOS 20 兼容基线）、macOS ARM64 CI；核心代码不包含平台专用推理逻辑。

## 快速使用 HTTP 服务

发布包解压后，在包目录运行：

Windows：

```powershell
.\lw-ppocr-http-service.exe
```

Linux：

```bash
chmod +x lw-ppocr-http-service
./lw-ppocr-http-service
```

统信 UOS 20 ARM64 请下载文件名包含 `linux-arm64-uos20` 的产物。该包使用 Debian 10
ARM64 的 glibc 2.28 / GCC 8.3 基线原生构建，部署和兼容边界见
[Linux ARM64 与统信 UOS 20](docs/LINUX-ARM64-UOS20.md)。

macOS Apple Silicon：

```bash
chmod +x run-http-service.sh
./run-http-service.sh
```

服务就绪后访问 <http://127.0.0.1:8787/>。启动时会固定输出作者、QQ、配置文件位置和全部有效参数；即使关闭 spdlog，也不会隐藏这段核对信息。QQ群只记录在本文档中，不在服务控制台和网页输出。

### Docker / Docker Compose

`v0.4.0` 提供 `linux/amd64` 容器镜像。发布后可直接运行：

```bash
docker run -d --name lw-ppocr --restart unless-stopped \
  -p 8787:8787 -v lw-ppocr-logs:/data/logs \
  ghcr.io/lxw112190/lw.ppocr.opencvdnn:0.4.0
```

使用 Compose：

```bash
cp .env.example .env
docker compose up -d
docker compose ps
```

在 `.env` 中设置 `PPOCR_API_KEY` 即可启用认证。镜像以非 root 用户运行，包含模型、OpenCV、网页与健康检查，日志文件保存在命名卷中。完整说明见 [docs/DOCKER.md](docs/DOCKER.md)。

### 安装为系统服务

Windows 请先编辑好 `http-service.json`，然后右键以管理员身份运行：

```bat
install-service.bat
stop-service.bat
restart-service.bat
start-service.bat
uninstall-service.bat
```

服务名为 `lw.PPOCR.OpenCVDNN`，默认设置为自动启动，异常退出后自动重启。安装脚本记录发布包当前绝对路径，因此安装后不要移动或删除解压目录；需要迁移时请先卸载，再从新目录重新安装。

Linux 使用 systemd，在发布包目录执行：

```bash
sudo ./install-service.sh
sudo ./stop-service.sh
sudo ./restart-service.sh
sudo ./start-service.sh
sudo ./uninstall-service.sh
```

服务名为 `lw-ppocr-opencvdnn.service`，默认使用执行 `sudo` 的用户运行并随系统启动。可在安装时通过 `LW_PPOCR_SERVICE_USER` 指定账户，例如 `sudo LW_PPOCR_SERVICE_USER=ocr ./install-service.sh`。日志仍写入发布包的 `logs/` 目录。

完整 OCR 推荐直接上传图片二进制，避免 Base64 大约 33% 的体积膨胀：

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: image/jpeg" \
  --data-binary @image.jpg
```

仍兼容 JSON/Base64：

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: application/json" \
  -d '{"image_base64":"..."}'
```

识别已裁剪的单行文字也支持二进制：

```bash
curl http://127.0.0.1:8787/api/recognize \
  -H "Content-Type: image/png" \
  --data-binary @cropped-text.png
```

批量识别已裁剪区域：

```json
{"images_base64":["...","..."]}
```

二进制单图请求接受 `image/*` 或 `application/octet-stream`。批量识别仍使用 JSON/Base64；`image_base64` 既可以是纯 Base64，也可以是浏览器产生的 `data:image/png;base64,...`。测试网页默认使用二进制接口。

完整请求、响应和状态码说明见 [docs/HTTP-API.md](docs/HTTP-API.md)。

### API Key

默认不启用。编辑 [http-service.json](http-service.json)，给 `api_key` 设置非空值并重启服务：

```json
{"api_key":"replace-with-a-long-random-secret"}
```

之后所有 OCR 请求必须携带：

```text
X-API-Key: replace-with-a-long-random-secret
```

测试网页也提供 API Key 输入框。服务不会把 Key、图片 Base64、请求体、识别文本写入日志。生产环境还应通过反向代理提供 HTTPS，并限制监听地址、防火墙和请求大小。

### 日志配置

```json
"logging": {
  "enabled": true,
  "level": "info",
  "console": true,
  "file_enabled": true,
  "file_path": "logs/runtime.log",
  "request_enabled": true,
  "request_start_enabled": true,
  "access_file_enabled": true,
  "access_file_path": "logs/access.log",
  "access_format": "text",
  "flush_interval_seconds": 1,
  "trusted_proxies": [],
  "max_file_size_mb": 10,
  "max_files": 5
}
```

- `runtime.log` 记录启动、模型加载、警告、异常与崩溃线索；`access.log` 默认使用与运行日志一致的带时间文本格式，记录请求 ID、状态、输入/输出大小、图片尺寸、分阶段耗时和错误码；需要日志采集程序解析时可改为 `jsonl`。
- `enabled=false` 会关闭全部 spdlog 日志；`request_enabled=false` 仅关闭逐请求访问日志与 `request_started`。
- `request_start_enabled=true` 会在推理前刷新一条运行日志，便于定位“处理哪一个请求时退出”；访问日志按 `flush_interval_seconds` 周期刷新，两类文件独立按大小轮转。
- 所有 OCR 响应都会在 JSON 和 `X-Request-ID` 响应头返回相同请求 ID。默认忽略 `X-Forwarded-For`；只有直接代理 IP 明确列入 `trusted_proxies` 后才会采用其转发地址。
- 已捕获异常、`std::terminate` 和 Windows 未处理异常代码会尽力写入并刷新日志。

日志不等于崩溃转储：断电、`kill -9`、严重内存破坏等场景无法保证留下完整信息或调用栈。生产环境建议同时启用 Windows dump 或 Linux `coredumpctl`。详见 [docs/LOGGING.md](docs/LOGGING.md)。

## C API

公共头文件只有 [include/lw/ppocr.h](include/lw/ppocr.h)。典型调用顺序：

1. `lw_ppocr_config_init`
2. 设置 `model_manifest_utf8`
3. `lw_ppocr_create`
4. 调用 `lw_ppocr_ocr_encoded` 或 `lw_ppocr_recognize_encoded`
5. 用 `lw_ppocr_string_free` 释放 JSON
6. `lw_ppocr_destroy`

完整示例见 [examples/c/main.c](examples/c/main.c)、[examples/csharp](examples/csharp) 和 [examples/python/ocr.py](examples/python/ocr.py)。C ABI v1 的兼容规则见 [docs/C-ABI.md](docs/C-ABI.md)，模型清单 Schema v1 的字段、校验与升级规则见 [docs/MODEL-MANIFEST.md](docs/MODEL-MANIFEST.md)。

C# 示例使用 .NET 8，通过同一套 C ABI 在 Windows/Linux 上调用：

```powershell
dotnet run --project examples/csharp -c Release -- `
  dist/local-win-x64 `
  models/ppocrv6-tiny/model.json `
  models/ppocrv6-tiny/sample.jpg `
  ocr
```

## 从源码构建

要求：CMake 3.24+、C++17 编译器、OpenCV 4.5+（`core`、`imgproc`、`imgcodecs`、`dnn`）。

项目保持 C++17：官方 CI 使用的 OpenCV 5 本身要求 C++17，项目也使用了 `std::filesystem` 等 C++17 能力。对外接口是稳定 C ABI，调用方无需采用 C++17。GCC 8 的 `std::filesystem` 会自动补充链接 `stdc++fs`，可覆盖统信 UOS 20 / Debian 10 时代的工具链。若必须支持只提供 C++11 工具链的旧系统，建议单独维护 OpenCV 4.x + C++11 的兼容分支。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR=/path/to/opencv
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/package
```

Windows 使用 DLL，Linux 使用 `.so`，macOS 使用 `.dylib`。CI 发布包按“解压即可运行”整理：包含 OCR Runtime、OpenCV、模型、配置、网页和示例；Windows 额外包含 VC143 x64 运行库，Linux 额外包含 `libstdc++.so.6` 与 `libgcc_s.so.1`，图像编解码依赖静态编入 OpenCV。glibc、Linux 动态加载器、Windows 系统 DLL 和 macOS 系统框架不随包分发，目标机仍需满足发布包标注的系统与架构基线。

Linux 与 macOS CI 会在 OpenCV 编译安装成功后保存缓存，后续相同缓存键的构建会直接复用；Windows CI 缓存官方预编译包的解压目录。四套原生工作流都会执行单元测试、模型文件 SHA-256、C ABI 导出、黄金正确性回归、HTTP 冒烟测试，以及多尺寸图片并发、异常请求和 RSS 内存增长测试。Linux ARM64 工作流还会验证 AArch64 ELF 与最高 GLIBC 符号版本。Windows 工作流每天定时执行 5000 次长测，普通提交执行 64 次快速稳定性检查。Docker 工作流额外验证非 root 运行、Compose、健康检查、API Key 和二进制 OCR；推送正式 `v*` 标签时才会发布 GHCR 镜像。所有发布附件均包含 SHA-256 校验文件。

## 并发建议

单个推理实例内部串行执行。HTTP 服务通过 `engine_instances` 创建多个独立实例实现并行；每增加一个实例会重复加载三套模型并增加内存占用。通常从 `engine_instances=1`、`worker_threads=4` 开始压测，再按 CPU 核数和内存调整。

## 许可证

项目代码采用 MIT License。第三方组件和模型的许可证见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) 与 `licenses/`。

## 联系与支持

- 作者：天天代码码天天
- QQ：819069052
- QQ Group: C# 人工智能实践 | 群号: 758616458
- 项目地址：<https://github.com/lxw112190/lw.PPOCR.OpenCVDNN>

如果项目对你有帮助，可以扫码支持维护：

<img src="docs/assets/sponsor.jpg" alt="捐赠二维码" width="240">
