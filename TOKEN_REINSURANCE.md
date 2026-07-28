# Token Reinsurance — `slvDev/esp32-ai`

> 本文档记录用于访问 `slvDev/esp32-ai` 仓库的 GitHub Personal Access Token (PAT) 的用途、权限范围和续期策略，属于团队级别的**令牌再保险**（Token Reinsurance）文档。
>
> ⚠️ **令牌值不写入此文件**。请使用环境变量 `GITHUB_TOKEN` 或密钥管理工具注入。

---

## 1. 令牌概览

| 项目 | 内容 |
|---|---|
| 令牌用途 | 访问 `slvDev/esp32-ai` 仓库（API / git over HTTPS） |
| 令牌类型 | GitHub Personal Access Token (PAT) — classic |
| 令牌所有者 | **rickqi** (GitHub User ID: 1237482) |
| 权限级别 | **`pull` (只读)** |
| 启用日期 | 创建时记录 |
| 过期策略 | 见 [§3 续期流程](#3-续期流程) |

### 1.1 权限范围

| Scope | 值 | 说明 |
|---|---|---|
| `repo` | 部分 | 仅 `public_repo`（公共仓库只读） |
| `read:org` | 是 | 读取组织成员身份 |
| `read:repo_hook` | 否 | 不涉及钩子 |

> 当前验证结果：`pull: true`，`push/maintain/admin` 均为 `false`。不可推送代码。

### 1.2 速率限制

| 类别 | 限制 | 已验证 |
|---|---|---|
| Core API | 5000 req/hr | ✅ 已验证 |
| Search API | 30 req/min | ✅ 已验证 |

---

## 2. 使用场景

### 2.1 CI/CD 拉取代码

```yaml
# GitHub Actions 示例
- name: Checkout
  uses: actions/checkout@v4
  with:
    token: ${{ secrets.GITHUB_TOKEN }}
```

> 在 GitHub Actions 中优先使用内置的 `GITHUB_TOKEN`，仅在需要跨仓库访问或触发后续事件流时使用此 PAT。

### 2.2 本地 CLI 身份认证

```powershell
# PowerShell —— 通过环境变量注入（推荐）
$env:GITHUB_TOKEN = "<从密钥管理工具复制>"

# 验证
gh auth status
```

```bash
# bash / Git Bash
export GITHUB_TOKEN="<从密钥管理工具复制>"
```

### 2.3 脚本/自动化中使用

```powershell
# 避免在代码中硬编码令牌
$headers = @{
    "Authorization" = "Bearer $env:GITHUB_TOKEN"
    "Accept" = "application/vnd.github.v3+json"
}
Invoke-RestMethod -Uri "https://api.github.com/repos/slvdev/esp32-ai" `
                  -Headers $headers
```

### 2.4 Git 远端认证（HTTPS）

如果使用 HTTPS 克隆私有仓库子模块，可以将 PAT 嵌入远端 URL（推荐使用环境变量代替明文）：

```bash
# 不推荐硬编码，推荐以下方式之一：

# 方式 1：使用凭证助手（推荐）
git config --global credential.helper "manager"

# 方式 2：环境变量注入
git clone https://$GITHUB_TOKEN@github.com/slvDev/esp32-ai.git

# 方式 3：使用 gh CLI 认证
gh auth login
```

---

## 3. 续期流程

### 3.1 检查令牌是否即将过期

```powershell
# 使用 GitHub API 检查
$headers = @{
    "Authorization" = "Bearer $env:GITHUB_TOKEN"
}
$authInfo = Invoke-RestMethod -Uri "https://api.github.com/user" -Headers $headers
Write-Output "Authenticated as: $($authInfo.login)"

# PAT classic 不会过期除非设置过期时间，但建议每 90 天主动轮换
```

### 3.2 轮换步骤

1. 登录 https://github.com/settings/tokens
2. 找到对应 PAT，**点击 Regenerate**（不要删除旧令牌直到新令牌已验证）
3. 选择相同权限（仅 `public_repo` 只读）
4. 复制新令牌
5. 更新所有使用方：
   - 本地 `.env` / 密钥管理工具
   - CI/CD secrets
   - 团队 Wiki / Onboarding 文档中的占位符说明
6. **验证**：
   ```powershell
   $headers = @{
       "Authorization" = "Bearer $env:GITHUB_TOKEN"
   }
   $repo = Invoke-RestMethod -Uri "https://api.github.com/repos/slvdev/esp32-ai" -Headers $headers
   Write-Output "Access: pull=$($repo.permissions.pull)"
   ```
7. **撤销旧令牌**（在上线确认后）—— 点击旧令牌的 `Delete` 按钮

### 3.3 应急吊销

如怀疑令牌泄露：

```powershell
# 立即吊销（不可恢复！）
# 通过 GitHub Web UI：
# 1. 访问 https://github.com/settings/tokens
# 2. 找到对应 PAT，点击 Delete

# 或通过 CLI 吊销所有本机认证：
gh auth logout
```

---

## 4. 安全注意事项

| 原则 | 说明 |
|---|---|
| ❌ 禁止硬编码 | 任何代码、配置文件、文档中都**不得**包含明文令牌值 |
| ✅ 使用环境变量 | `$env:GITHUB_TOKEN`（PowerShell）/ `$GITHUB_TOKEN`（bash） |
| 🔐 最小权限 | 当前为只读 pull 权限，永远不要为读场景分配写权限 |
| 🔄 定期轮换 | 每 90 天重新生成一次 |
| 📋 记录用途 | 每个 PAT 应有明确用途（此文档即为记录） |
| 🚪 离岗撤销 | 人员离开时立即吊销其关联的个人令牌 |
| 🔒 避免日志泄露 | 不要将 PAT 输出到终端日志或 CI 日志中 |
| 🏗️ 使用 GitHub App 或 OAuth App | 长期运行的自动化建议使用 GitHub App 而非个人令牌 |

---

## 5. 常见问题

### Q: 令牌过期后仓库还能拉取吗？

本地缓存的 Git 凭据可能继续工作一段时间，但新的克隆或 CI 拉取会失败。续期前不要删除旧令牌。

### Q: 这个文档为什么叫 "Token Reinsurance"？

类比再保险（Reinsurance）：再保险是保险公司的保险；Token Reinsurance 是令牌管理的第二道防线——当令牌泄露、过期、或责任人离岗时，这篇文档就是应急预案和执行手册。

### Q: PAT 只能用于这个仓库吗？

是的，该 PAT 的 scope 仅限 `public_repo`，仅能访问公共仓库。对 `slvDev/esp32-ai` 仓库的权限为只读 pull。

### Q: 谁拥有这个 PAT？

当前 PAT 归属于 GitHub 用户 **rickqi**（ID: 1237482）。如需转移至团队共享账户或机器人账户，请联系管理员创建 GitHub App 或机器用户。

### Q: 为什么不用 Fine-grained PAT？

Fine-grained PAT 支持更细粒度的权限控制（仅限特定仓库、特定权限），且可以设置过期时间。建议下次创建时改用 Fine-grained PAT 并设置 90 天过期。

---

## 6. 附录：GitHub Token 类型对比

| 类型 | 适用场景 | 过期时间 | 管理方式 |
|---|---|---|---|
| PAT classic | 个人 CLI / 脚本 | 不自动过期 | 手动续期 |
| Fine-grained PAT | 仓库级别精细化权限 | ⚠️ 可选过期 | 组织/仓库级管控 |
| GitHub App Token | CI/CD / 长期自动化 | 1 小时（可刷新） | 自动续期 |
| OAuth Token | Web 应用 / 三方集成 | 按配置 | 手动刷新 |
| `GITHUB_TOKEN` | Actions 工作流内使用 | 工作流结束 | 自动创建/销毁 |

---

> 最后更新：2026-07-28
> 维护人：rickqi