# C ABI v1 compatibility contract / C ABI v1 兼容契约

`LW_PPOCR_API_VERSION == 1` is frozen starting with v0.3.0. The authoritative
header is `include/lw/ppocr.h`; `tests/abi/exports-v1.txt` is the frozen export
baseline (`docs/exports-v1.txt` in a release package).

从 v0.3.0 开始，`LW_PPOCR_API_VERSION == 1` 正式冻结。权威公共头文件为
`include/lw/ppocr.h`，导出函数基线为 `tests/abi/exports-v1.txt`（发布包中为
`docs/exports-v1.txt`）。

## Compatibility rules / 兼容规则

- Existing exported functions cannot be removed, renamed, reordered at the
  binary level, or changed in calling convention, argument types, return type,
  ownership, or error semantics while API version 1 is active.
- `lw_ppocr_version` and `lw_ppocr_config` field order, alignment, meaning, and
  size are frozen for API v1. Reserved fields must remain zero when supplied by
  callers and may only acquire documented meaning in a compatible release.
- Status and log-level numeric values are frozen. New values may be appended;
  existing values cannot change.
- A breaking change requires a new API version and new ABI baseline. Product
  version changes such as 0.3.x do not by themselves change the ABI version.
- The library accepts encoded image bytes only for the duration of a call. JSON
  returned by OCR functions is owned by the library and must be released with
  `lw_ppocr_string_free` from the same loaded library.
- `lw_ppocr_destroy` receives a pointer to the handle and clears it. Calling it
  with a null pointer or an already-null handle is safe.
- Calls on one engine handle are internally serialized. Use separate handles
  for parallel inference when the available memory has been measured.

- API v1 有效期间，不得删除或重命名已有导出函数，也不得修改调用约定、参数、
  返回值、内存所有权和错误语义。
- `lw_ppocr_version` 与 `lw_ppocr_config` 的字段顺序、对齐、含义和大小冻结；
  调用方必须把保留字段置零。
- 状态码和日志级别的既有数值冻结，只允许追加新值。
- 破坏性变更必须升级 API version 并建立新的 ABI 基线；产品版本升级不等于 ABI 升级。
- 输入图片字节只需在调用期间有效；返回 JSON 必须使用同一动态库的
  `lw_ppocr_string_free` 释放。
- 同一个引擎句柄的推理调用会在内部串行化；需要并行时请创建独立句柄并评估内存。

## Automated checks / 自动检查

`tests/api_test.cpp` freezes public constants, function signatures, structure
layout, defaults, and version reporting. `scripts/check_abi.py` loads every
packaged DLL, `.so`, or `.dylib` and verifies all ABI v1 exports. These checks
run on Windows x64, Linux x64, and macOS ARM64 CI.

`tests/api_test.cpp` 固定公共常量、函数签名、结构布局、默认值和版本报告；
`scripts/check_abi.py` 会加载发布包里的动态库并逐一验证 ABI v1 导出符号。
Windows x64、Linux x64 与 macOS ARM64 CI 都会执行这些检查。
