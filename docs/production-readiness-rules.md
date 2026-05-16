# PingPong 生产化阶段执行规则

本文档从以下三份文档中提炼通用规则，用于指导后续生产化阶段、重构任务和实现 PR 的推进：

- `docs/phase-execution-workflow.md`
- `docs/production-readiness-plan.md`
- `docs/production-readiness-status-rule.md`

## 1. 适用范围

本文档适用于：

- 生产化 Phase 的实现与验收
- 与生产化相关的重构任务
- 行为变更、测试补充、README 更新和状态更新
- 跨会话继续推进时的计划状态维护

对于轻量重构任务，也应尽量遵循本文档的最小闭环：

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
- 状态规则文档或相关计划状态已同步更新

## 4. 标准执行流程

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

- 生产计划大文档更新
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

如果 `mergeable: false`：

1. 从最新 `main` 重新创建干净实现分支
2. 只迁移必要代码和测试变更
3. 关闭有问题的旧 PR
4. 重新创建新 PR

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
- mergeable=false 强行推进
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

## 5. 生产化准入方向

后续生产化工作应持续围绕以下准入方向推进。

### 5.1 协议鲁棒性

- CRC 错误不应立即导致 Master fail
- 非当前 seq 包应可忽略并统计
- 未知 type 包应可忽略并统计
- 非本网络、非本设备目标包应可忽略并统计
- 认证失败包应可忽略并统计
- 重放包应可忽略并统计
- Ping 冲突只统计，不立即失败
- 超时与重试行为应可配置、可观测

### 5.2 配置能力

- 支持编译期默认配置
- 支持运行时配置覆盖默认配置
- 支持查询默认配置并基于默认值局部修改
- 默认值集中维护，避免示例、测试和核心逻辑重复定义

### 5.3 设备身份

- 包内应包含 `network_id`
- 包内应包含 `src_id`
- 包内应包含 `dst_id`
- 设备只处理目标网络和目标地址的包
- 广播地址默认关闭

### 5.4 认证与防重放

- 生产目标需要认证与防重放能力，不能只依赖 CRC
- MAC 建议使用 SipHash-2-4，并输出 64-bit 认证码
- 认证输入应覆盖 version、type、flags、seq、network_id、src_id、dst_id、counter / nonce
- 重复 counter 或旧 counter 包应被拒绝并统计
- CRC 继续用于误码检测，但不作为安全机制

### 5.5 可观测性

- 统计字段应覆盖成功、失败、重试、CRC、parse、冲突、RX timeout、TX timeout
- 应增加地址过滤、认证失败、重放拒绝相关统计
- 应支持查询最近失败原因
- 应支持查询最近收包信息，如 RSSI、SNR、RTT、seq、src_id、dst_id
- 可选 trace 应能定位状态迁移

### 5.6 测试与硬件验证

- 单元测试覆盖率保持既定门禁要求
- 覆盖 Master 忽略坏包后继续等待的行为
- 覆盖默认配置与运行时覆盖行为
- 覆盖自动重启行为
- 覆盖地址过滤、网络过滤、认证失败、重放拒绝
- 覆盖序列号回绕和长时间 process 调用稳定性
- 正式生产前应完成 24 小时和 72 小时硬件压力测试

## 6. Tag 规则

Alpha tag 只能在对应 Phase 的实现 PR 已合并到 `main` 后创建。

要求：

- tag 必须指向已通过 CI 的 `main` 提交
- tag 创建后，相关里程碑状态才能从 `[ ]` 更新为 `[√]`

当前规划中的 Alpha tag：

```text
v0.1.0-alpha  Phase 1 后
v0.2.0-alpha  Phase 4 后
v0.3.0-alpha  Phase 5 后
v0.4.0-alpha  Phase 6 后
```

## 7. 禁止事项汇总

- 禁止从文档计划分支直接开实现 PR
- 禁止一个 PR 同时实现多个 Phase
- 禁止 CI 未通过就合并
- 禁止 mergeable=false 时强行推进
- 禁止只新增测试而不更新已有旧行为断言
- 禁止把计划状态提前标记为完成
- 禁止合并到 `main` 后跳过 README 检查
- 禁止把计划文档大更新混入功能实现 PR

## 8. 面向新增重构任务的模板

新增重构任务建议使用以下格式：

```text
## Phase X：任务名称 [ ]

目标：

当前行为：

目标行为：

任务：

验收标准：

建议优先级：

分支建议：

PR 标题建议：

README 影响：
```

示例：

```text
## Phase X：统一运行期状态调度 [ ]

目标：收敛运行期状态切换，降低状态机维护风险。

当前行为：start、stop、process、tx_done、rx_done 等路径分散修改状态。

目标行为：除 init/reset 外，运行期状态切换只通过 pp_dispatch() 完成。

任务：新增 EVT_TICK、pp_event_data_t、pp_dispatch()，并改造相关 API 为事件投递。

验收标准：运行期函数不直接写 pp->state；相关状态迁移测试通过；public API 和包格式不变。

建议优先级：最高。

分支建议：refactor/runtime-state-dispatch

PR 标题建议：refactor: unify runtime state dispatch

README 影响：如无用户可见 API 变化，应记录已检查 README，无需更新。
```
