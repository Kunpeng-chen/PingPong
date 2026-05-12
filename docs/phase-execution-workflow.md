# PingPong 后续阶段标准执行流程

本文档记录 Phase 2 成功跑通后的标准工作流。后续 Phase 3 及之后的所有任务，默认都必须按此流程执行。

## 1. 核心原则

每个阶段必须使用“干净实现分支 + 独立 PR + CI 通过 + squash merge”的流程推进，避免把计划文档分支、历史临时提交或无关变更混入实现 PR。

## 2. 标准流程

### Step 1：从最新 `main` 创建干净实现分支

不要直接从 `docs/production-readiness-plan` 或其他文档分支开发功能。

分支命名建议：

```text
feat/phaseN-short-feature-name
```

示例：

```text
feat/phase2-master-bad-packet-tolerance
```

## Step 2：只提交当前 Phase 必要变更

实现 PR 中只包含当前 Phase 的代码、测试和必要文档说明。

禁止混入：

- 生产计划大文档更新
- 与当前 Phase 无关的重构
- 历史临时分支上的旧提交
- 多个 Phase 的功能打包提交

## Step 3：同步更新所有现有测试预期

如果当前 Phase 改变了行为，必须同步更新已有测试文件，不能只新增专项测试。

Phase 2 的经验：

- 新增了 `test_master_bad_packet_tolerance`
- 同步更新了 `test_ping_pong_migrated.c`
- 同步更新了 `test_ping_pong_extensions.c`

否则即使新测试通过，旧测试也可能因为旧行为断言而导致 CI 失败。

## Step 4：创建 PR 到 `main`

PR 必须满足：

- base branch：`main`
- head branch：当前 Phase 的干净实现分支
- PR 描述包含行为变更、测试覆盖、范围说明
- PR 中不要夹带计划文档分支的历史提交

## Step 5：检查 PR 可合并性

创建 PR 后必须检查：

- `mergeable: true`
- 无冲突提示
- changed files 只包含当前 Phase 必要文件

如果出现 `mergeable: false`，优先采用 Phase 2 中验证过的修复方式：

1. 从最新 `main` 重新创建干净实现分支
2. 只迁移必要代码和测试变更
3. 关闭有问题的旧 PR
4. 重新创建新 PR

## Step 6：等待并检查 CI

必须确认 GitHub Actions CI 完成且成功：

```text
status: completed
conclusion: success
```

Phase 2 的通过记录：

- PR：#10
- CI workflow：`CI`
- run number：`60`
- conclusion：`success`

## Step 7：使用 squash merge 合并

CI 通过且 PR 可合并后，使用 squash merge 合并到 `main`。

Phase 2 的合并记录：

```text
PR #10: feat: make master tolerate corrupted packets until timeout
merge commit: 066c75e05b04a1de1334f5fc64a0903e38e22d11
```

## Step 8：合并后更新计划状态

只有当以下条件全部满足后，才能把对应 Phase 标记为 `[√]`：

- 实现 PR 已合并到 `main`
- CI 已通过
- 相关测试已覆盖
- 无遗留冲突 PR
- 状态规则文档已更新

## 3. 后续阶段统一要求

从 Phase 3 开始，每个阶段都按以下节奏执行：

1. 明确当前 Phase 的最小交付范围
2. 从最新 `main` 创建干净实现分支
3. 实现代码
4. 新增专项测试
5. 更新旧测试预期
6. 创建 PR 到 `main`
7. 检查 `mergeable: true`
8. 等待 CI `success`
9. squash merge
10. 更新计划状态

## 4. 禁止事项

- 禁止从文档计划分支直接开实现 PR
- 禁止一个 PR 同时实现多个 Phase
- 禁止 CI 未通过就合并
- 禁止 mergeable=false 时强行推进
- 禁止只新增测试而不更新已有旧行为断言
- 禁止把计划状态提前标记为完成

## 5. 推荐提交/PR 命名

提交命名：

```text
feat: ...
test: ...
docs: ...
```

PR 命名：

```text
feat: phaseN short feature description
```

示例：

```text
feat: make master tolerate corrupted packets until timeout
feat: add compile-time default config header
feat: add master auto-restart mode
```
