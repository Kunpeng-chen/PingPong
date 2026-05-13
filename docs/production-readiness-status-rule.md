# PingPong 计划跨会话状态规则

为了便于跨会话继续推进，每个计划标题尾部必须添加状态标记。

## 标记规则

- `[ ]`：未完成或进行中
- `[√]`：已完成

当某个计划项完成并验证通过后，将该计划标题尾部从 `[ ]` 更新为 `[√]`。

后续新建计划项时，也必须默认在标题尾部添加 `[ ]`。

## 当前推荐状态

- Phase 1：恢复测试体系与 CI 稳定性 [√]
- Phase 2：增强 Master 对坏包的容错 [√]
- Phase 3：编译期默认配置与运行时覆盖 [√]
- Phase 4：Master 自动连续运行与失败恢复策略 [√]
- Phase 5：引入设备地址和网络 ID [ ]
- Phase 6：认证与防重放 [ ]
- Phase 7：硬件压力测试与现场试点 [ ]

## 推荐 PR 状态

1. `[√]` `test: migrate legacy tests to current notification model`
2. `[√]` `release: tag v0.1.0 baseline after test restoration`
3. `[√]` `feat: make master tolerate corrupted/unrelated packets until timeout`
4. `[√]` `feat: add compile-time default config header`
5. `[√]` `feat: add master auto-restart mode`
6. `[ ]` `feat: add identity fields and versioned v2 packet format`
7. `[ ]` `feat: add SipHash-2-4 authentication and replay protection`

## Alpha Tag 里程碑

- Phase 1 完成后：创建 `v0.1.0-alpha` [√]
- Phase 4 完成后：创建 `v0.2.0-alpha` [ ]
- Phase 5 完成后：创建 `v0.3.0-alpha` [ ]
- Phase 6 完成后：创建 `v0.4.0-alpha` [ ]

Tag 创建规则：

- 只有对应 Phase 的实现 PR 已合并到 `main` 后，才允许创建 tag。
- Tag 必须指向已通过 CI 的 `main` 提交。
- 创建 tag 后，将对应里程碑尾部从 `[ ]` 更新为 `[√]`。

## 标准执行流程

后续所有 Phase 默认使用 `docs/phase-execution-workflow.md` 中记录的标准流程推进。

核心规则：

1. 从最新 `main` 创建干净实现分支。
2. 实现 PR 只包含当前 Phase 的必要代码、测试和少量说明。
3. 不从 `docs/production-readiness-plan` 等文档分支直接开实现 PR。
4. 如果 PR 出现 `mergeable: false`，重新从 `main` 创建干净分支，只迁移必要变更。
5. PR 必须确认 `mergeable: true`。
6. CI 必须完成且 `conclusion: success`。
7. 使用 squash merge 合并到 `main`。
8. 合并后再更新本状态文档。

Phase 2 已验证该流程：

- 干净实现分支：`feat/phase2-master-bad-packet-tolerance`
- 成功 PR：#10
- CI：`CI` run #60，`success`
- 合并方式：squash merge
- merge commit：`066c75e05b04a1de1334f5fc64a0903e38e22d11`

Phase 3 已验证该流程：

- 干净实现分支：`feat/phase3-compile-time-default-config`
- 成功 PR：#12
- CI：`CI` run #63，`success`
- 合并方式：squash merge
- merge commit：`03a142f3d3903f3b98205a98a247981a66ed47c9`

Phase 4 已验证该流程：

- 干净实现分支：`feat/phase4-master-auto-restart`
- 成功 PR：#13
- CI：`CI` run #66，`success`
- 合并方式：squash merge
- merge commit：`9e9a0c6437e2b39789dce58f372c454c172850a9`

## 新增配置计划说明

`feat: add compile-time default config header` 的目标是引入 `src/ping_pong_config.h`，形成“编译期默认配置 + 运行时覆盖配置”的双层模型。

推荐规则：

- `ping_pong_init()` 自动加载 `ping_pong_config.h` 中的默认宏。
- 用户未调用 `ping_pong_set_config()` 时，使用默认宏配置。
- 用户调用 `ping_pong_set_config()` 时，运行时配置覆盖默认配置。
- 新增 `ping_pong_get_default_config()`，方便用户基于默认值局部修改。
- 后续 `auto_restart` 与 `restart_delay_ms` 的默认值也应放入 `ping_pong_config.h`。
