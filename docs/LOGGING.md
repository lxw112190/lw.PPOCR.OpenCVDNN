# Logging and crash diagnostics / 日志与崩溃诊断

v0.4.0 separates human-readable runtime diagnostics from OCR access records.
JSON Lines is the default access format for stable collection and analysis,
while the timestamped text format remains available for manual reading. The
design borrows Nginx's operational strengths without
using the web-oriented `combined` format.

v0.4.0 将便于人工阅读的运行日志与便于采集分析的 OCR 访问日志分离。设计吸收了
Nginx 的运维优点，但没有照搬面向网页访问的 `combined` 格式。

## Files / 文件

- `logs/runtime.log`: startup, model loading, warnings, errors, crashes, and
  optional `request_started` breadcrumbs.
- `logs/access.log`: one JSON Lines completion record per OCR request by
  default; the timestamped text style can be enabled explicitly.
- Console output contains runtime messages and access records when
  `logging.console=true`. Startup parameters remain human-readable.

- `logs/runtime.log`：启动、模型加载、警告、异常、崩溃以及可选的
  `request_started` 线索。
- `logs/access.log`：每个 OCR 请求完成后默认记录一行 JSON；也可显式启用带时间前缀的文本格式。
- `logging.console=true` 时，两类日志也会输出到控制台；启动参数始终保持人工可读。

## Configuration / 配置

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
  "access_format": "jsonl",
  "flush_interval_seconds": 1,
  "trusted_proxies": [],
  "max_file_size_mb": 10,
  "max_files": 5
}
```

`access_format` accepts `jsonl` (default) or `text`. Runtime and access file
paths must be different. Both files use independent size-based rotation with
the same size and retention settings.

`access_format` 只能为 `jsonl`（默认）或 `text`。运行日志和访问日志必须使用不同
文件；二者分别按 `max_file_size_mb` 和 `max_files` 轮转。

Text access records use the same prefix as `runtime.log`, including local time,
level, and thread ID:

```text
[2026-08-01 14:23:35.485] [info] [23876] request_id=... remote_ip=127.0.0.1 peer_ip=127.0.0.1 method=POST path=/api/ocr operation=ocr content_type=image/jpeg request_bytes=46855 response_bytes=5365 status=200 duration_ms=159.12 results=16
```

文本访问日志与 `runtime.log` 使用相同前缀，包含本地时间、日志级别和线程 ID。
JSONL 模式不添加文本前缀，以保证每一行都是可直接解析的合法 JSON。

## JSONL access schema v1 / JSONL 访问日志 Schema v1

The machine-readable contract is frozen in
`schemas/access-log-v1.schema.json`; its checksum is protected by
`schemas/contracts-v1.lock.json`. JSON Lines is the default operational format;
set `access_format` to `text` only when human readability is preferred over the
structured contract.

机器可读契约冻结在 `schemas/access-log-v1.schema.json`，其校验值由
`schemas/contracts-v1.lock.json` 保护。JSON Lines 是默认运维格式；仅在更重视人工
阅读而不需要结构化契约时，才显式把 `access_format` 设为 `text`。

```json
{"log_schema_version":1,"timestamp":"2026-08-01T07:14:28.645Z","level":"info","event":"request_complete","request_id":"657f059abc868-1","remote_ip":"127.0.0.1","peer_ip":"127.0.0.1","method":"POST","path":"/api/ocr","operation":"ocr","content_type":"image/jpeg","request_format":"binary","status":200,"request_bytes":62496,"response_bytes":3287,"result_count":16,"duration_ms":183.94,"image":{"width":500,"height":500},"timing_ms":{"decode":1.21,"detector":91.32,"classifier":8.16,"recognizer":73.40,"pipeline":174.09,"server_total":183.94}}
```

Stable fields include request/peer addresses, endpoint operation, media format,
status, request and response sizes, result count, total duration, optional image
dimensions, and available OCR stage timings. Failed requests add a stable
`error_code`, such as `unauthorized`, `invalid_json`, `invalid_request`,
`payload_too_large`, or `internal_error`.

稳定字段包括请求 ID、客户端与直接连接地址、操作、媒体类型、状态码、请求/响应
大小、结果数、总耗时、可用的图片尺寸和 OCR 阶段耗时。失败请求额外包含稳定的
`error_code`。

Every `/api/ocr` and `/api/recognize` response returns the same request ID in
both the JSON body and `X-Request-ID` response header. `/health` also returns an
`X-Request-ID` header.

每个 OCR 响应都会在 JSON 与 `X-Request-ID` 响应头中返回同一个请求 ID，便于客户
报错时直接定位日志；`/health` 也会返回该响应头。

## Trusted proxies / 可信代理

Forwarded client addresses are ignored by default. Add only the exact IP address
of a reverse proxy that you control:

```json
"trusted_proxies": ["127.0.0.1", "::1"]
```

Only when the immediate peer matches this list does the service accept the
first valid IP from `X-Forwarded-For` as `remote_ip`. The socket peer is always
kept as `peer_ip`. CIDR ranges and wildcard entries are intentionally not
accepted in Schema v1.

默认忽略所有转发地址。只有直接连接来源与 `trusted_proxies` 中的精确 IP 一致时，
服务才会采用 `X-Forwarded-For` 的第一个合法 IP 作为 `remote_ip`；实际连接地址始终
记录为 `peer_ip`。当前不接受 CIDR 或通配符，避免错误配置导致伪造客户端地址。

## Flush and crash behavior / 刷盘与崩溃行为

- Runtime warning, error, and critical records flush immediately.
- Access records are periodically flushed using `flush_interval_seconds`.
- When `request_start_enabled=true`, `request_started` is written to the runtime
  log and explicitly flushed after authentication but before decoding and
  inference. Unauthorized requests are recorded only in the access log and
  cannot force a synchronous runtime-log flush. Disable this option
  when maximum throughput matters more than identifying the in-flight request
  before a native crash.
- All logs are flushed during a graceful shutdown.

- warning/error/critical 运行日志立即刷盘。
- access 日志按 `flush_interval_seconds` 周期刷盘。
- 开启 `request_start_enabled` 时，会在解码和推理前把 `request_started` 明确刷盘；
  该操作在鉴权通过后执行，未授权请求只写访问日志，不能强制同步刷盘。极致性能场景
  可以关闭它，但异常退出后将无法保证找到当时正在处理的请求。
- 正常停止服务时会刷新全部日志。

## Privacy / 隐私

Logs never contain API Keys, authorization headers, request bodies, Base64 image
data, encoded image bytes, or recognized text. Treat IP addresses and operational
metadata according to the deployment's privacy and retention requirements.

日志不会记录 API Key、鉴权请求头、请求体、Base64、图片字节或识别文本。IP 地址和
运维元数据仍应按照部署环境的隐私与保留策略管理。

## Limits / 限制

A log is not a crash dump. Power loss, `kill -9`, severe memory corruption, disk
failure, and some operating-system terminations can still prevent a final write.
Use Windows Error Reporting or ProcDump on Windows and `coredumpctl`/core dumps
on Linux. Correlate the dump with the final flushed `request_started` request ID.

日志不能替代崩溃转储。断电、`kill -9`、严重内存破坏或磁盘故障仍可能导致最后记录
丢失。Windows 建议配合 WER/ProcDump，Linux 建议配合 `coredumpctl` 或 core dump。
