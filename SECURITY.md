# Security policy / 安全策略

## Supported versions / 支持版本

`v1.0.0` is the first stable release. Security fixes target the latest
published 1.x minor release while the frozen C ABI v1 and machine-readable v1
contracts remain backward compatible throughout the 1.x line. This policy is
not an indefinite LTS commitment; supported versions may change with a future
major release and will be documented here before that change.

`v1.0.0` 是首个正式稳定版本。安全修复面向最新发布的 1.x 次版本提供，同时在整个
1.x 周期内保持已冻结的 C ABI v1 与机器可读 v1 契约向后兼容。这不是无限期 LTS
承诺；若未来主版本调整支持范围，会先在本文档中明确说明。

## Reporting / 报告方式

Please do not disclose a suspected vulnerability in a public issue. Use the
repository's GitHub **Security > Report a vulnerability** page. If private
reporting is unavailable, contact the author by QQ `819069052` and include only
enough information to arrange a private channel; do not send secrets, customer
images, API Keys, or exploitable details in a public group.

疑似漏洞请勿直接发布到公开 Issue。优先使用仓库的 GitHub
**Security > Report a vulnerability** 私密报告入口；若该入口不可用，请通过
QQ `819069052` 联系作者建立私密沟通渠道。不要在公开群聊中发送密钥、客户图片、
API Key 或可直接利用的漏洞细节。

Include the affected version/platform, reproduction steps, impact, and any
suggested mitigation. Acknowledgement and remediation timing depend on severity
and reproducibility.

报告中请包含受影响版本与平台、复现步骤、影响范围以及可选的缓解建议。确认和修复
时间取决于严重程度与可复现性。

## Scope / 范围

The project protects its checked-in dependencies with hashes, publishes a
CycloneDX SBOM, runs CodeQL, and reviews dependency changes. These controls do
not replace TLS, authentication, network isolation, OS patching, or least-
privilege deployment by operators.

项目通过依赖哈希、CycloneDX SBOM、CodeQL 和依赖变更审查保护供应链；这些措施
不能替代部署方的 TLS、认证、网络隔离、操作系统补丁和最小权限配置。
