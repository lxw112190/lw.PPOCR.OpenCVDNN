# Frozen v1 contracts / 已冻结的 v1 契约

v0.6.0 freezes three public machine-readable contracts:

- HTTP API v1: `schemas/http-api-v1.openapi.json`
- HTTP service configuration Schema v1:
  `schemas/http-service-config-v1.schema.json`
- JSONL access-log Schema v1: `schemas/access-log-v1.schema.json`

v0.6.0 冻结上述 HTTP API、服务配置和 JSONL 访问日志三套 v1 契约。

`schemas/contracts-v1.lock.json` records the SHA-256 of every frozen document.
`scripts/validate_contracts.py` validates the example configuration and rejects
unreviewed modifications to those files. A breaking change must be introduced
as a new versioned v2 document and endpoint or negotiated contract; do not edit
a frozen v1 file in place. Backward-compatible clarifications should normally
be documented without changing the schema.

`schemas/contracts-v1.lock.json` 保存所有冻结文件的 SHA-256；
`scripts/validate_contracts.py` 会验证示例配置并阻止未审查的契约变更。破坏兼容性的
修改必须新增 v2 文件以及相应端点或协商机制，不能直接改写 v1。仅用于解释、不改变
机器行为的兼容性说明应优先写入文档。

All JSON responses include `X-LW-PPOCR-API-Version: 1`. OCR responses also
include `X-Request-ID`, matching the JSON `request_id`. Error responses expose
a stable `error_code`; human-readable `error` text is diagnostic and must not be
used as a programmatic contract.

所有 JSON 响应都包含 `X-LW-PPOCR-API-Version: 1`。OCR 响应还返回与 JSON
`request_id` 一致的 `X-Request-ID`。错误响应的 `error_code` 是稳定字段，供程序
判断；可读的 `error` 文本只用于诊断，不应作为程序契约。

The configuration root contains `schema_version: 1`. Unknown fields and
unsupported schema versions are rejected at startup so misspellings do not
silently weaken production controls. `access_format: "text"` is the default;
set it to `jsonl` when the frozen machine-readable access-log schema is needed.

配置根节点包含 `schema_version: 1`。服务启动时会拒绝未知字段和不支持的版本，避免
配置拼写错误被静默忽略。`access_format: "text"` 是默认值；需要使用冻结的机器可读
日志 Schema 时，应显式设置为 `jsonl`。
