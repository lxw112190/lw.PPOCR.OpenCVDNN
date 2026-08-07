# Supply-chain controls / 供应链控制

v0.7.0 introduces four complementary controls:

1. `dependencies.lock.json` records the version, license, source, package URL,
   version markers, deterministic SHA-256 tree hashes of vendored code/models,
   and the expected OpenCV release commit, source archive hash, and Windows
   archive hash.
2. `scripts/validate_dependencies.py` detects an unreviewed file, content,
   version-marker, or license change. UTF-8 text is canonicalized to LF before
   hashing for stable results across Windows, Linux, and macOS; binary files
   are hashed byte-for-byte.
3. `sbom/lw.PPOCR.OpenCVDNN.cdx.json` is a deterministic CycloneDX 1.6 SBOM and
   is included in release packages and uploaded as a CI artifact.
4. GitHub Actions runs CodeQL C/C++, dependency review on pull requests, and
   scheduled supply-chain verification.

v0.7.0 使用四类互补控制：锁定依赖与模型哈希、验证任何未审查变化、生成并分发
CycloneDX 1.6 SBOM，以及执行 CodeQL、PR 依赖审查和定时供应链验证。UTF-8 文本
在哈希前统一为 LF，保证 Windows、Linux 和 macOS 得到相同摘要；二进制文件仍逐字节校验。

Validate before committing a legitimate dependency update:

```bash
python3 scripts/validate_dependencies.py
python3 scripts/generate_sbom.py --version 1.1.0 \
  --output sbom/lw.PPOCR.OpenCVDNN.cdx.json
```

When intentionally updating a dependency, first review its upstream release,
license, source authenticity, and security advisories. Then update the vendored
files, notices/licenses, version markers, tree hash, and generated SBOM in one
commit. Never “fix” a hash mismatch without explaining and reviewing the file
change that caused it.

有意升级依赖时，应先审查上游发布、许可证、来源真实性和安全公告，再在同一个提交中
更新 vendored 文件、许可证声明、版本标记、树哈希和 SBOM。不能在没有解释并审核
实际文件变化的情况下，只修改哈希来消除报错。

The SBOM identifies what the project distributes; it is not itself a guarantee
that every component is vulnerability-free. Operators should combine it with
their own vulnerability feed, deployment inventory, patch policy, and the
guidance in `SECURITY.md`.

SBOM 用于准确说明项目分发了什么，并不保证所有组件永远没有漏洞。部署方仍应结合
自己的漏洞情报、资产清单、补丁策略与 `SECURITY.md` 执行持续治理。
