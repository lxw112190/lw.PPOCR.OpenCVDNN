# Linux ARM64 and UnionTech UOS 20 / Linux ARM64 与统信 UOS 20

The ARM64 package is built natively on an AArch64 GitHub-hosted runner inside
a Debian 10 userland. This intentionally matches the principal toolchain ABI
baseline observed on UnionTech UOS Desktop 20 Professional ARM64:

- Architecture: AArch64 / ARMv8-A
- glibc: 2.28
- GCC: 8.3
- OpenCV DNN: 5.0.0, built for generic ARMv8-A
- Inference device: CPU

ARM64 发布包在 GitHub 原生 AArch64 Runner 上、Debian 10 用户态中完成编译，目标是
匹配统信 UOS Desktop 20 Professional ARM64 常见的 ABI 基线：glibc 2.28、GCC 8.3、
ARMv8-A。OpenCV 5.0.0、OCR 动态库、HTTP 服务、模型、网页、GCC 运行库和服务脚本
均包含在发布包中。

## CI package / CI 产物

```text
lw.PPOCR.OpenCVDNN-v1.1.0-linux-arm64-uos20.tar.gz
lw.PPOCR.OpenCVDNN-v1.1.0-linux-arm64-uos20.tar.gz.sha256
```

The workflow validates the native architecture, maximum required glibc symbol
version, unit tests, real OCR, the C ABI, JSONL/text access logs, malformed
requests, concurrency, and RSS growth. `BUILD-ENVIRONMENT.txt` records the
actual architecture, glibc, GCC, and OpenCV versions used for that artifact.

CI 会验证 AArch64 ELF、最高 GLIBC 符号版本、单元测试、真实 OCR、C ABI、JSONL/Text
访问日志、异常请求、并发和 RSS 内存增长。包内的 `BUILD-ENVIRONMENT.txt` 记录实际
构建环境，便于部署前核对。

## Run / 运行

```bash
uname -m
getconf GNU_LIBC_VERSION
sha256sum -c lw.PPOCR.OpenCVDNN-v1.1.0-linux-arm64-uos20.tar.gz.sha256
tar -xzf lw.PPOCR.OpenCVDNN-v1.1.0-linux-arm64-uos20.tar.gz
cd lw.PPOCR.OpenCVDNN-v1.1.0-linux-arm64-uos20
chmod +x run-http-service.sh
./run-http-service.sh
```

`uname -m` must report `aarch64`. The target glibc must be 2.28 or newer. Open
`http://127.0.0.1:8787/` after the service reports ready. For LAN deployment,
change `listen_host`, configure a strong API Key, and restrict the port with a
firewall or reverse proxy.

`uname -m` 必须输出 `aarch64`，目标系统 glibc 必须为 2.28 或更高版本。服务启动成功
后访问 `http://127.0.0.1:8787/`。局域网部署时还应设置强 API Key，并通过防火墙或
反向代理限制访问。

## systemd

```bash
./install-service.sh --verify-only
sudo ./install-service.sh
sudo ./restart-service.sh
sudo ./stop-service.sh
sudo ./uninstall-service.sh
```

`--verify-only` asks `systemd-analyze` to parse the generated unit without
installing it. The installer uses the absolute path of the target machine's
`bash` and writes `WorkingDirectory=` with systemd path escaping but without
outer quotes, which is verified on openEuler 22.03 SP1/systemd 249. On a start
failure it prints the installed unit, full service status, and the latest 80
journal records.

## Compatibility statement / 兼容性声明

Debian 10 provides a reproducible public ABI baseline but is not an official
UnionTech image. UOS editions and service packs can differ in system libraries,
CPU adaptations, and security updates. Therefore CI establishes a strong UOS
20 compatibility target, while a formal “verified on UOS” claim still requires
running the downloaded package on the exact licensed UOS edition in scope.

Debian 10 CI 提供的是可复现的公开 ABI 兼容基线，并不是统信官方镜像。不同 UOS
桌面版、服务器版、补丁版本和 CPU 适配版可能存在差异，因此 CI 结论应表述为
“面向 UOS 20 ARM64 的兼容构建”；只有在指定统信实体机上完成部署验证后，才能升级
为该具体版本的“实体机已验证”。

ARMv7/ARM32, LoongArch64, SW64, MIPS64, and x86_64 require different binaries
and are not covered by this package.

该包不支持 ARMv7/ARM32、LoongArch64、SW64、MIPS64 或 x86_64，这些架构必须使用
各自独立编译的发布包。
