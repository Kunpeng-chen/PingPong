# PingPong 运行期状态调度重构计划

## Phase X：统一运行期状态调度 [ ]

目标：

收敛 PingPong 运行期状态切换，解决 `start`、`stop`、`process`、`tx_done`、`rx_done` 等路径分散修改状态的问题，降低状态机维护风险。

本次重构保持轻量级设计：

- 不拆分源文件
- 不改变 public API
- 不改变协议包格式
- 不引入 `pp_enter_xx()` 状态入口函数
- 只新增统一事件调度入口 `pp_dispatch()`

当前行为：

- `ping_pong_start()` 直接修改 `pp->state`，进入 `TX` 或 `RX_WAIT`
- `ping_pong_stop()` 直接修改 `pp->state`，进入 `STOPPED`
- `ping_pong_process()` 内部根据超时、重试、失败恢复等逻辑间接触发状态变化
- `ping_pong_on_tx_done()` 通过 `enter_rx_wait()` 进入 `RX_WAIT`
- `ping_pong_on_rx_done()` 根据收到的包、角色和解析结果触发成功、失败、冲突或重新等待
- 状态切换分散在多个函数和 helper 中，审查状态行为需要跨函数追踪

目标行为：

- 新增统一运行期状态调度函数：

```c
static ping_pong_err_t pp_dispatch(
    ping_pong_t *pp,
    pp_event_t event,
    const pp_event_data_t *data
);
```

- public API 只负责参数检查、构造事件数据并投递事件
- 运行期状态切换只允许发生在 `pp_dispatch()` 中
- `init` 和 `reset` 保留生命周期初始化 / 强制清零语义，允许直接设置初始状态
- `stop` 属于运行期事件，应进入 `pp_dispatch()`
- `valid_transitions` 的合法性校验应收敛到 `pp_dispatch()` 内部

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

2. 扩展内部事件枚举，新增 `EVT_TICK`。

3. 新增 `pp_dispatch()`，集中处理运行期状态迁移。

4. 改造以下 API 为事件投递入口：

```text
ping_pong_start()      -> EVT_START_MASTER / EVT_START_SLAVE
ping_pong_stop()       -> EVT_STOP
ping_pong_process()    -> EVT_TICK
ping_pong_on_tx_done() -> EVT_TX_DONE
ping_pong_on_rx_done() -> EVT_RX_DONE
```

5. 清理运行期函数中的直接 `pp->state = xxx`。

6. 保留 `init/reset` 中的直接状态初始化逻辑。

7. 保持现有通知行为、统计字段和协议包格式不变。

8. 更新或新增单元测试，覆盖新的状态调度路径。

不做什么：

- 不拆分 `ping_pong.c`
- 不新增 `ping_pong_fsm.c`、`ping_pong_packet.c` 等文件
- 不引入 `pp_enter_tx()` / `pp_enter_rx_wait()` 等状态入口函数
- 不调整 v1 6 字节包格式
- 不引入 v2 identity、认证、防重放等生产化功能
- 不修改用户侧 public API

验收标准：

- 除 `ping_pong_init()` 和 `ping_pong_reset()` 外，运行期状态切换只发生在 `pp_dispatch()` 中
- `ping_pong_start()`、`ping_pong_stop()`、`ping_pong_process()`、`ping_pong_on_tx_done()`、`ping_pong_on_rx_done()` 不直接写 `pp->state`
- 原有单元测试全部通过
- 新增或更新测试覆盖：
  - Master start 进入 TX
  - Slave start 进入 RX_WAIT
  - TX_DONE 后进入 RX_WAIT
  - RX timeout 后 Master retry
  - retry 达到上限后 Master fail
  - STOP 可从 TX / RX_WAIT 进入 STOPPED
  - 非法状态事件返回 `PING_PONG_ERR_INVALID_STATE`
  - RX_DONE 成功 Pong 路径
  - RX_DONE 坏包统计并继续等待路径
- `ctest --output-on-failure` 全部通过
- coverage 保持既定门禁要求
- public API 不变
- 协议包格式不变
- README 已检查；如无用户可见变化，执行记录中明确“已检查 README，无需更新”

建议优先级：最高。

分支建议：

```text
refactor/runtime-state-dispatch
```

PR 标题建议：

```text
refactor: unify runtime state dispatch
```

README 影响：

本次重构主要是内部状态调度收敛，预计不改变用户侧 API 和接入方式。

如果实现过程中未产生用户可见变化，合并后应记录：

```text
已检查 README，无需更新。
```

如果实现过程中调整了行为说明、调试建议或状态机描述，则需要同步更新 `README.md` 或相关源码文档。
