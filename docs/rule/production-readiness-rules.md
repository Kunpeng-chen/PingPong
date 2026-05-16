# PingPong 阶段执行通用规则

本文档用于约束后续阶段任务、重构任务和实现 PR 的通用执行流程。

具体计划内容、阶段目标、功能路线图和生产化准入细节应放在 `docs/plan/` 下；本文件只记录通用流程、状态标记和协作约束。

## 1. 适用范围

本文档适用于：

- 阶段任务的实现与验收
- 重构任务的推进
- 行为变更、测试补充、README 更新和状态更新
- 跨会话继续推进时的计划状态维护

最小闭环：

```text
明确范围 -> 干净分支 -> 实现与测试 -> PR -> CI -> squash merge -> README 检查 -> 状态更新
```

## 2. 核心原则

1. 每个阶段必须使用干净实现分支推进。
2. 实现分支必须从最新 `main` 创建。
3. 一个 PR 只实现一个 Phase 或一个明确的独立任务。
4. PR 中只包含当前任务必要的代码、测试和少量说明。
5. 不得混入无关重构、历史临时提交或多个 Phase 的功能。
6. CI 未通过不得合并。
7. PR 不可合并时不得强行推进，应修复错误内容直至成功。
8. 合并到 `main` 后必须检查 README。
9. 计划状态不得提前标记完成。
10. 一个计划必须根据复杂度拆成多个可独立验收的阶段，不能默认只写一个 Phase。

## 3. 计划状态标记规则

所有计划标题尾部必须添加状态标记：

```text
[ ]  未完成或进行中
[√]  已完成
```

新增计划项默认使用 `[ ]`。

只有满足以下条件后，才允许将 `[ ]` 更新为 `[√]`：

- 实现 PR 已合并到 `main`
- CI 已完成且结果为 `success`
- 相关测试已覆盖
- README 已更新，或已明确记录“已检查 README，无需更新”
- 无遗留冲突 PR
- 相关计划状态已同步更新

## 4. 计划文档命名规则

具体计划文档必须放在 `docs/plan/` 下。

命名格式：

```text
<scope>-<topic>-plan.md
```

命名要求：

- 使用小写英文
- 单词之间使用短横线 `-`
- 文件名必须以 `-plan.md` 结尾
- 文件名应体现计划范围和主题
- 不使用空格、中文、下划线或日期前缀

示例：

```text
docs/plan/production-readiness-plan.md
docs/plan/runtime-state-dispatch-plan.md
docs/plan/protocol-v2-identity-plan.md
docs/plan/security-replay-protection-plan.md
```

如果计划是某个 Phase 的实现计划，可以使用：

```text
docs/plan/phase5-identity-v2-packet-plan.md
```

如果计划是跨 Phase 的专题计划，使用专题名，不强制带 Phase 编号。

## 5. 计划拆分规则

制定计划时，必须先根据复杂度拆分为多个可独立验收的阶段。

拆分原则：

- 每个 Phase 只解决一个明确问题
- 每个 Phase 应能独立提交 PR、独立测试、独立验收
- 高风险改动应先拆出测试保护或行为基线 Phase
- 行为变化和结构调整应尽量分开
- 计划文档可以覆盖多个 Phase，但实现 PR 不应一次打包多个 Phase
- 简单任务可以只有一个 Phase，但必须在计划中说明为什么不需要拆分

推荐拆分维度：

```text
测试保护 -> 内部结构调整 -> 行为迁移 -> 清理收尾 -> 文档更新
```

## 6. 计划优先级规则

当存在多个计划或多个未完成 Phase 时，agent 不应在 `AGENTS.md` 中硬编码当前优先计划，而应根据计划文档和以下规则判断优先级。

优先级从高到低：

1. 用户当前明确指定的计划或 Phase。
2. 阻塞其他计划继续推进的测试、CI、规则或流程修复。
3. 已开始但未完成，且后续 Phase 依赖它的计划。
4. 计划文档中标记为“建议优先级：最高”的未完成 Phase。
5. 能降低后续实现风险的测试保护、行为基线或状态收敛工作。
6. 计划文档中标记为“建议优先级：高”的未完成 Phase。
7. 普通增强、清理和文档完善工作。

如果多个计划优先级相同，应优先选择：

- 依赖链更靠前的 Phase
- 风险更低、可独立验收的 Phase
- 与用户当前上下文更相关的 Phase

如果仍无法判断，应说明候选项和取舍依据，再继续推进最小、最安全的下一步。

## 7. 标准执行流程

### Step 1：明确当前阶段最小交付范围

每个 Phase 或任务开始前，必须先明确：

- 目标
- 当前行为
- 目标行为
- 任务范围
- 不做什么
- 验收标准

### Step 2：从最新 `main` 创建干净实现分支

分支命名建议：

```text
feat/phaseN-short-feature-name
refactor/phaseN-short-feature-name
fix/phaseN-short-feature-name
```

示例：

```text
feat/phase5-identity-v2-packet
refactor/runtime-state-dispatch
```

### Step 3：只提交当前阶段必要变更

允许包含：

- 当前任务代码实现
- 当前任务相关测试
- 必要 README 或简短文档说明

禁止包含：

- 计划文档大更新
- 无关重构
- 历史临时分支旧提交
- 多个 Phase 的功能打包提交

### Step 4：同步更新测试预期

如果当前任务改变了既有行为，必须同步更新已有测试，不能只新增专项测试。

要求：

- 新增专项测试覆盖新行为
- 更新旧测试中不再正确的旧行为断言
- 保持 `ctest --output-on-failure` 全部通过
- 覆盖率保持既定门禁要求

### Step 5：创建 PR 到 `main`

PR 要求：

- base branch：`main`
- head branch：当前干净实现分支
- PR 描述包含行为变更、测试覆盖和范围说明
- PR changed files 只包含当前任务必要文件

PR 标题建议：

```text
feat: phaseN short feature description
refactor: short refactor description
fix: short fix description
```

### Step 6：检查 PR 可合并性

创建 PR 后必须检查：

- `mergeable: true`
- 无冲突提示
- changed files 范围正确

如果 PR 不可合并，应修复错误内容直至成功，不得强行推进。

### Step 7：等待并检查 CI

必须确认 GitHub Actions CI：

```text
status: completed
conclusion: success
```

CI 失败时，应优先修复当前 PR，不得绕过检查合并。

### Step 8：使用 squash merge 合并

CI 通过且 PR 可合并后，使用 squash merge 合并到 `main`。

禁止：

- CI 未通过合并
- PR 不可合并时强行推进
- 将多个 Phase 混合后一次合并

### Step 9：合并后检查 README

每一次实现 PR 合并到 `main` 后，都必须检查 `README.md`。

如果有用户可见变化，README 至少应同步：

- 当前主分支新增能力
- 关键行为变化
- 新增或调整后的测试 / 示例入口
- 对用户接入有影响的配置、接口或注意事项

如果没有 README 可见变化，也应在执行记录中明确：

```text
已检查 README，无需更新。
```

### Step 10：更新计划状态

满足完成条件后，才允许将对应计划项从 `[ ]` 更新为 `[√]`。

## 8. Tag 规则

Alpha tag 只能在对应 Phase 的实现 PR 已合并到 `main` 后创建。

要求：

- tag 必须指向已通过 CI 的 `main` 提交
- tag 创建后，相关里程碑状态才能从 `[ ]` 更新为 `[√]`

具体 tag 规划应记录在 `docs/plan/` 下的计划文档中。

## 9. 禁止事项汇总

- 禁止一个 PR 同时实现多个 Phase
- 禁止 CI 未通过就合并
- 禁止 PR 不可合并时强行推进
- 禁止只新增测试而不更新已有旧行为断言
- 禁止把计划状态提前标记为完成
- 禁止合并到 `main` 后跳过 README 检查
- 禁止把计划文档大更新混入功能实现 PR
- 禁止把具体计划内容写入 `docs/rule/` 下的通用规则文档
- 禁止复杂计划只写一个笼统 Phase
- 禁止在 `AGENTS.md` 中硬编码当前最高优先级计划

## 10. 面向新增任务的模板

新增任务建议使用以下格式，并放入 `docs/plan/` 下的具体计划文档中：

```text
# 任务名称计划

## 1. 总目标

## 2. 拆分依据

说明该计划为什么拆成这些 Phase；如果只有一个 Phase，说明不需要拆分的原因。

## Phase 1：阶段名称 [ ]

目标：

当前行为：

目标行为：

任务：

不做什么：

验收标准：

建议优先级：

分支建议：

PR 标题建议：

README 影响：

## Phase 2：阶段名称 [ ]

...
```
