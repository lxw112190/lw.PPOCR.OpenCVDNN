# Docker / Docker Compose

The official container currently targets `linux/amd64` and uses the OpenCV DNN
CPU backend. It includes OpenCV 5.0.0, the PP-OCRv6 Tiny models, the HTTP
service, and the browser page.

## 中文说明

### 使用预构建镜像

发布 `v0.7.0` 后，可直接运行 GitHub Container Registry 镜像：

```bash
docker run -d \
  --name lw-ppocr \
  --restart unless-stopped \
  -p 8787:8787 \
  -v lw-ppocr-logs:/data/logs \
  ghcr.io/lxw112190/lw.ppocr.opencvdnn:0.7.0
```

访问 <http://127.0.0.1:8787/>。查看状态和日志：

```bash
docker inspect --format '{{json .State.Health}}' lw-ppocr
docker logs -f lw-ppocr
```

### Docker Compose

复制示例环境变量并按需编辑：

```bash
cp .env.example .env
docker compose up -d
docker compose ps
docker compose logs -f ppocr
```

从当前源码自行构建：

```bash
docker build --platform linux/amd64 \
  -t ghcr.io/lxw112190/lw.ppocr.opencvdnn:0.7.0 .
docker compose up -d
```

停止服务：

```bash
docker compose down
```

日志文件保存在命名卷 `ppocr-logs`。只有明确不再需要日志时才执行
`docker compose down -v`。

### API Key

在 `.env` 中设置：

```dotenv
PPOCR_API_KEY=replace-with-a-long-random-secret
```

重新创建容器：

```bash
docker compose up -d --force-recreate
```

请求时增加请求头：

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "X-API-Key: replace-with-a-long-random-secret" \
  -H "Content-Type: image/jpeg" \
  --data-binary @image.jpg
```

API Key 不会写入启动输出或请求日志。启动输出只显示
`configured / 已配置 (value hidden / 明文已隐藏)`。

### Compose 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `PPOCR_IMAGE` | `ghcr.io/lxw112190/lw.ppocr.opencvdnn:0.7.0` | 镜像名称和标签 |
| `PPOCR_PORT` | `8787` | 宿主机映射端口 |
| `PPOCR_API_KEY` | 空 | 空值表示不启用认证 |
| `PPOCR_ENGINE_INSTANCES` | `1` | 独立模型实例数量，增加后会显著增加内存 |
| `PPOCR_WORKER_THREADS` | `4` | HTTP 工作线程数量 |
| `PPOCR_LOGGING_ENABLED` | `true` | 总日志开关 |
| `PPOCR_FILE_LOGGING_ENABLED` | `true` | `/data/logs` 文件日志开关 |
| `PPOCR_REQUEST_LOGGING_ENABLED` | `true` | 逐请求日志开关 |

镜像内部固定监听 `0.0.0.0:8787`。若只允许本机访问，可将 Compose
端口映射改成 `127.0.0.1:8787:8787`。

## English

### Prebuilt image

After the `v0.7.0` image is published:

```bash
docker run -d \
  --name lw-ppocr \
  --restart unless-stopped \
  -p 8787:8787 \
  -v lw-ppocr-logs:/data/logs \
  ghcr.io/lxw112190/lw.ppocr.opencvdnn:0.7.0
```

Open <http://127.0.0.1:8787/>. The container runs as the non-root UID/GID
`10001`, includes a health check, and stores rotating file logs in the named
volume.

### Compose

```bash
cp .env.example .env
docker compose up -d
docker compose ps
docker compose logs -f ppocr
```

Set `PPOCR_API_KEY` in `.env` to enable authentication. Send that value in the
`X-API-Key` header for `/api/ocr` and `/api/recognize` requests. Run
`docker compose down` to stop the service without deleting its log volume.

## Native service environment overrides

The same variables can be applied directly to the native HTTP executable:

| Variable | Purpose |
| --- | --- |
| `LW_PPOCR_LISTEN_HOST`, `LW_PPOCR_PORT` | Listen endpoint |
| `LW_PPOCR_MODEL_MANIFEST`, `LW_PPOCR_WEB_ROOT` | Model manifest and web root paths |
| `LW_PPOCR_API_KEY` | API Key; an empty value disables authentication |
| `LW_PPOCR_ENGINE_INSTANCES`, `LW_PPOCR_WORKER_THREADS` | Concurrency controls |
| `LW_PPOCR_MAX_REQUEST_BYTES`, `LW_PPOCR_MAX_IMAGE_PIXELS` | Input limits |
| `LW_PPOCR_LOGGING_ENABLED` | Master logging switch |
| `LW_PPOCR_CONSOLE_LOGGING_ENABLED` | Console logging switch |
| `LW_PPOCR_FILE_LOGGING_ENABLED`, `LW_PPOCR_LOG_FILE` | Runtime rotating-file switch and path |
| `LW_PPOCR_REQUEST_LOGGING_ENABLED` | OCR access-record master switch |
| `LW_PPOCR_REQUEST_START_LOGGING_ENABLED` | Pre-inference runtime breadcrumb switch |
| `LW_PPOCR_ACCESS_FILE_LOGGING_ENABLED`, `LW_PPOCR_ACCESS_LOG_FILE` | Access rotating-file switch and path |
| `LW_PPOCR_ACCESS_LOG_FORMAT` | Access format: `text` (default) or `jsonl` |
| `LW_PPOCR_LOG_FLUSH_INTERVAL_SECONDS` | Periodic flush interval, 1–60 seconds |
| `LW_PPOCR_TRUSTED_PROXIES` | Comma-separated exact proxy IPs trusted for `X-Forwarded-For` |

Environment values override `http-service.json`. Invalid integers, booleans,
paths, or values outside the existing safety limits cause startup to fail with
a clear error instead of silently falling back.
