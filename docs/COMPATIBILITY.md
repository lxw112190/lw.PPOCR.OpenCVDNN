# Compatibility matrix / 兼容矩阵

This matrix describes the release artifacts produced by CI. “CI verified”
means build, native tests, real-model OCR, HTTP/API regression, malformed-input
tests, ABI checks, and packaging all run on the listed baseline. It does not
claim compatibility with every newer operating-system combination.

本表描述 CI 生成的正式发布包。“CI 验证”表示在对应基线上执行构建、原生测试、
真实模型 OCR、HTTP/API 回归、异常输入、ABI 检查和打包；不代表已经覆盖所有更新的
操作系统组合。

| Artifact / 产物 | Architecture | Build and runtime baseline / 构建与运行基线 | OpenCV | Verification / 验证 |
| --- | --- | --- | --- | --- |
| `windows-x64` | x86-64 | Windows Server 2022 CI; intended for supported Windows 10/11 x64 | 5.0.0 | CI verified |
| `linux-x64` | x86-64 | Ubuntu 20.04, glibc 2.31 | 5.0.0 | CI verified |
| `linux-arm64-uos20` | AArch64 | Debian 10, glibc 2.28, GCC 8.3; UOS 20 ABI baseline | 5.0.0 | CI verified and UOS 20 physical machine verified / CI 与统信 UOS 20 实机验证 |
| `macos-arm64` | Apple Silicon | macOS 15 CI, deployment target macOS 12.0 | 5.0.0 | CI verified |
| Docker `linux/amd64` | x86-64 | Ubuntu 20.04 runtime image, non-root user | 5.0.0 | CI/Compose verified |

The project requires a C++17 compiler when building from source and supports
OpenCV 5.0 or newer at source level; OpenCV 4.x is intentionally unsupported.
Official downloadable packages use the fixed OpenCV 5.0.0 dependency set
recorded in `dependencies.lock.json`.

源码构建要求支持 C++17 和 OpenCV 5.0 及以上版本，明确不支持 OpenCV 4.x。正式
下载包固定使用 `dependencies.lock.json` 记录的 OpenCV 5.0.0 依赖组合。

Linux packages bundle OpenCV, `libstdc++.so.6`, and `libgcc_s.so.1`, but do not
bundle glibc or the ELF loader. The target machine must use the same CPU
architecture and a glibc version no older than the artifact baseline. macOS
artifacts are ad-hoc signed rather than notarized; Gatekeeper policy may require
an explicit local approval. Windows packages bundle the VC143 runtime but still
depend on standard Windows system components.

Linux 包内包含 OpenCV、`libstdc++.so.6` 和 `libgcc_s.so.1`，但不打包 glibc 与
ELF 加载器；目标机必须架构一致，且 glibc 不低于产物基线。macOS 产物使用 ad-hoc
签名而非 Apple 公证，Gatekeeper 策略可能需要用户明确放行。Windows 包包含 VC143
运行库，但仍依赖标准 Windows 系统组件。
