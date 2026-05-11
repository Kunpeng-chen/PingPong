# PingPong 计划跨会话状态规则

为了便于跨会话继续推进，每个计划标题尾部必须添加状态标记。

## 标记规则

- `[ ]`：未完成或进行中
- `[√]`：已完成

当某个计划项完成并验证通过后，将该计划标题尾部从 `[ ]` 更新为 `[√]`。

后续新建计划项时，也必须默认在标题尾部添加 `[ ]`。

## 当前推荐状态

- Phase 1：恢复测试体系与 CI 稳定性 [√]
- Phase 2：增强 Master 对坏包的容错 [ ]
- Phase 3：编译期默认配置与运行时覆盖 [ ]
- Phase 4：Master 自动连续运行与失败恢复策略 [ ]
- Phase 5：引入设备地址和网络 ID [ ]
- Phase 6：认证与防重放 [ ]
- Phase 7：硬件压力测试与现场试点 [ ]

## 推荐 PR 状态

1. `[√]` `test: migrate legacy tests to current notification model`
2. `[√]` `release: tag v0.1.0 baseline after test restoration`
3. `[ ]` `feat: make master tolerate corrupted/unrelated packets until timeout`
4. `[ ]` `feat: add compile-time default config header`
5. `[ ]` `feat: add master auto-restart mode`
6. `[ ]` `feat: add identity fields and versioned v2 packet format`
7. `[ ]` `feat: add SipHash-2-4 authentication and replay protection`

## 新增配置计划说明

`feat: add compile-time default config header` 的目标是引入 `src/ping_pong_config.h`，形成“编译期默认配置 + 运行时覆盖配置”的双层模型。

推荐规则：

- `ping_pong_init()` 自动加载 `ping_pong_config.h` 中的默认宏。
- 用户未调用 `ping_pong_set_config()` 时，使用默认宏配置。
- 用户调用 `ping_pong_set_config()` 时，运行时配置覆盖默认配置。
- 新增 `ping_pong_get_default_config()`，方便用户基于默认值局部修改。
- 后续 `auto_restart` 与 `restart_delay_ms` 的默认值也应放入 `ping_pong_config.h`。
