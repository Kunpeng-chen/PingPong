# PingPong 运行期状态调度重构计划

## 1. 总目标

收敛 PingPong 运行期状态切换，解决 `start`、`stop`、`process`、`tx_done`、`rx_done` 等路径分散修改状态的问题，降低状态机维护风险。

本次重构保持轻量级设计：

- 不拆分源文件
- 不改变 public API
- 不改变协议包格式
- 不引入 `pp_enter_xx()` 状态入口函数
- 只新增统一事件调度入口 `pp_dispatch()`

## 2. 拆分依据

该重构会影响多个运行期入口和状态迁移路径，不能作为一个笼统 Phase 一次完成。

拆分原则：

- 先补测试保护，再迁移实现
- 先迁移简单路径，再迁移复杂路径
- 先收敛 start / stop / tx_done，再收敛 process / rx_done
- 每个 Phase 都应能独立提交 PR、独立测试、独立验收
- 行为保持不变，主要目标是结构收敛

## Phase 1：补充状态迁移保护测试 [ ]

目标：

在重构前补齐关键状态迁移测试，锁定现有行为，降低后续迁移风险。

当前行为：

已有测试覆盖部分 Master / Slave 流程，但状态切换分散，缺少针对非法事件、stop 路径、超时 retry/fail 等状态迁移的集中测试。

目标行为：

在不改实现逻辑的前提下，补充状态迁移测试，作为后续重构安全网。

任务：

1. 补充 Master start 进入 `TX` 的测试。
2. 补充 Slave start 进入 `RX_WAIT` 的测试。
3. 补充 `TX_DONE` 后进入 `RX_WAIT` 的测试。
4. 补充 STOP 可从 `TX` / `RX_WAIT` 进入 `STOPPED` 的测试。
5. 补充 RX timeout 后 Master retry 的测试。
6. 补充 retry 达到上限后 Master fail 的测试。
7. 补充非法状态事件返回 `PING_PONG_ERR_INVALID_STATE` 的测试。

不做什么：

- 不修改状态机实现
- 不新增 `pp_dispatch()`
- 不调整包解析逻辑
- 不改变 public API

验收标准：

- 新增测试全部通过
- 原有测试全部通过
- `ctest --output-on-failure` 全部通过
- coverage 保持既定门禁要求
- 无用户可见行为变化

建议优先级：最高。

分支建议：

```text
refactor/phase1-runtime-dispatch-tests
```

PR 标题建议：

```text
test: add runtime state transition coverage
```

README 影响：

预计无用户可见变化。合并后应记录：

```text
已检查 README，无需更新。
```

## Phase 2：引入 pp_dispatch 并迁移简单运行期入口 [ ]

目标：

新增 `pp_dispatch()` 和 `pp_event_data_t`，先迁移 `start`、`stop`、`on_tx_done` 三类较简单路径。

当前行为：

`ping_pong_start()`、`ping_pong_stop()`、`ping_pong_on_tx_done()` 会直接或间接修改 `pp->state`。

目标行为：

上述运行期入口只负责参数检查、构造事件数据并投递事件；对应状态切换由 `pp_dispatch()` 完成。

任务：

1. 新增事件数据结构：

```c
typedef struct {
    uint32_t now_ms;
    const uint8_t *rx_data;
    uint32_t rx_len;
    int16_t rssi;
    int16_t snr;
} pp_event_data_t;
```

2. 新增 `pp_dispatch()`。
3. 将 `valid_transitions` 校验收敛到 `pp_dispatch()` 内部。
4. 将 `ping_pong_start()` 改为投递 `EVT_START_MASTER` / `EVT_START_SLAVE`。
5. 将 `ping_pong_stop()` 改为投递 `EVT_STOP`。
6. 将 `ping_pong_on_tx_done()` 改为投递 `EVT_TX_DONE`。
7. 保留 `init/reset` 中的直接状态初始化逻辑。

不做什么：

- 不迁移 `ping_pong_process()` 的超时逻辑
- 不迁移 `ping_pong_on_rx_done()` 的收包逻辑
- 不改变通知行为
- 不改变 public API

验收标准：

- `start`、`stop`、`on_tx_done` 不直接写 `pp->state`
- 相关状态切换只发生在 `pp_dispatch()` 中
- Phase 1 测试全部通过
- 原有测试全部通过
- public API 不变
- 协议包格式不变

建议优先级：最高。

分支建议：

```text
refactor/phase2-runtime-dispatch-simple-events
```

PR 标题建议：

```text
refactor: route simple runtime events through dispatch
```

README 影响：

预计无用户可见变化。合并后应记录：

```text
已检查 README，无需更新。
```

## Phase 3：迁移 process 超时与自动重启调度 [ ]

目标：

将 `ping_pong_process()` 中的运行期状态变化收敛到 `pp_dispatch()`。

当前行为：

`ping_pong_process()` 内部直接处理 auto restart、TX timeout、RX timeout、Master retry、Master fail、Slave re-enter RX 等逻辑，并间接触发状态变化。

目标行为：

`ping_pong_process()` 只负责读取当前时间并投递 `EVT_TICK`；超时、重试、失败、自动重启等运行期状态迁移由 `pp_dispatch()` 处理。

任务：

1. 扩展内部事件枚举，新增 `EVT_TICK`。
2. 将 auto restart pending 检查迁移到 `pp_dispatch(EVT_TICK)`。
3. 将 TX timeout 处理迁移到 `pp_dispatch(EVT_TICK)`。
4. 将 Master RX timeout retry/fail 处理迁移到 `pp_dispatch(EVT_TICK)`。
5. 将 Slave RX timeout re-enter RX 处理迁移到 `pp_dispatch(EVT_TICK)`。
6. 保持现有统计更新和通知行为不变。

不做什么：

- 不迁移 RX_DONE 收包解析路径
- 不改变 auto_restart 语义
- 不改变 retry 次数语义
- 不改变超时配置语义

验收标准：

- `ping_pong_process()` 不直接或间接分散执行状态切换，只投递 `EVT_TICK`
- auto restart、TX timeout、RX timeout、retry、fail 测试全部通过
- Phase 1 / Phase 2 测试全部通过
- 原有测试全部通过
- public API 不变

建议优先级：最高。

分支建议：

```text
refactor/phase3-runtime-dispatch-tick
```

PR 标题建议：

```text
refactor: route process timeout handling through dispatch
```

README 影响：

预计无用户可见变化。合并后应记录：

```text
已检查 README，无需更新。
```

## Phase 4：迁移 rx_done 收包路径 [ ]

目标：

将 `ping_pong_on_rx_done()` 中的运行期状态变化收敛到 `pp_dispatch()`。

当前行为：

`ping_pong_on_rx_done()` 同时负责参数检查、包解析、角色分支、统计更新和状态变化。Master / Slave 收到合法包、坏包、冲突包时的状态处理分散在该函数内部。

目标行为：

`ping_pong_on_rx_done()` 只负责参数检查、构造事件数据并投递 `EVT_RX_DONE`；收包后的状态迁移由 `pp_dispatch()` 处理。

任务：

1. 将 `ping_pong_on_rx_done()` 改为投递 `EVT_RX_DONE`。
2. 将 Master 收到合法 Pong 的 success 路径迁移到 `pp_dispatch()`。
3. 将 Master 收到 CRC / parse / unknown / conflict 包的统计与继续等待路径迁移到 `pp_dispatch()`。
4. 将 Slave 收到 Ping 后进入 TX 的路径迁移到 `pp_dispatch()`。
5. 将 Slave 收到 CRC / unknown / Pong conflict 后重新等待的路径迁移到 `pp_dispatch()`。
6. 保持现有包格式、CRC 算法和统计语义不变。

不做什么：

- 不重构 packet codec
- 不新增 v2 包格式
- 不改变坏包容错策略
- 不改变 Master / Slave 对外行为

验收标准：

- `ping_pong_on_rx_done()` 不直接写 `pp->state`
- RX_DONE 相关状态迁移只发生在 `pp_dispatch()` 中
- 合法 Pong success 路径测试通过
- 坏包统计并继续等待路径测试通过
- Slave Ping -> Pong 路径测试通过
- 原有测试全部通过

建议优先级：高。

分支建议：

```text
refactor/phase4-runtime-dispatch-rx-done
```

PR 标题建议：

```text
refactor: route rx done handling through dispatch
```

README 影响：

预计无用户可见变化。合并后应记录：

```text
已检查 README，无需更新。
```

## Phase 5：清理状态切换残留并更新文档状态 [ ]

目标：

完成收尾清理，确保运行期状态切换已经真正收敛，并同步计划状态。

当前行为：

完成前几个 Phase 后，可能仍存在旧 helper、重复状态校验或残留的直接 `pp->state = xxx`。

目标行为：

除 `ping_pong_init()` 和 `ping_pong_reset()` 外，运行期状态切换只发生在 `pp_dispatch()` 中。

任务：

1. 全量检查 `ping_pong.c` 中的 `pp->state =` 写入位置。
2. 删除不再需要的状态 helper 或重复 transition 校验。
3. 确认 `valid_transitions` 只由 `pp_dispatch()` 使用。
4. 补充必要注释，明确状态切换边界。
5. 跑完整测试和覆盖率。
6. 检查 README 是否需要更新。
7. 满足完成条件后，将本计划中已完成 Phase 标记为 `[√]`。

不做什么：

- 不引入新的协议能力
- 不做 packet codec 拆分
- 不做 v2 identity / security
- 不做额外架构抽象

验收标准：

- 除 `init/reset` 外，运行期状态切换只存在于 `pp_dispatch()`
- 所有 Phase 测试全部通过
- `ctest --output-on-failure` 全部通过
- coverage 保持既定门禁要求
- public API 不变
- 协议包格式不变
- README 已检查；如无用户可见变化，执行记录中明确“已检查 README，无需更新”
- 本计划状态已按规则更新

建议优先级：高。

分支建议：

```text
refactor/phase5-runtime-dispatch-cleanup
```

PR 标题建议：

```text
refactor: clean up runtime state dispatch boundaries
```

README 影响：

如果只是内部清理，预计无需更新 README；但需要明确记录检查结果。
