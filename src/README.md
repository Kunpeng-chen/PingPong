# PingPong 中间件设计说明

PingPong 只负责协议核心：状态机、收发时序、超时与重试、冲突检测和基础统计。硬件访问、日志、事件分发和业务决策都由上层端口适配承担。

## 一句话

- 这是一个面向嵌入式场景的 Ping-Pong 协议核心模块
- 同一实例可配置为 `MASTER` 或 `SLAVE`
- 通过端口回调注入时间、通知和调试能力

## 边界

- 应由 PingPong 处理：状态切换、Ping / Pong 编解码、超时、重试、冲突检测、统计
- 不应由 PingPong 处理：具体硬件收发、时间源、日志、事件总线、业务决策

## 状态与角色

| 状态 | 含义 |
|------|------|
| `IDLE` | 已初始化，尚未启动 |
| `TX` | 正在发送 |
| `RX_WAIT` | 正在等待接收 |
| `STOPPED` | 已停止 |

| 角色 | 含义 |
|------|------|
| `NONE` | 未确定 |
| `MASTER` | 主机，主动发起 Ping，并负责重试 |
| `SLAVE` | 从机，被动等待 Ping 并回 Pong |

## 端口契约

- `get_time_ms`：必需，返回当前毫秒时间戳
- `notify`：必需，接收协议通知
- `user_data`：透传给 `notify`
- `trace`：可选调试钩子

建议 `notify` 只做轻量处理，不要在回调里直接重入调用 PingPong API。

## 公共 API

| 分组 | API |
|------|-----|
| 初始化与配置 | `ping_pong_instance_size` `ping_pong_init` `ping_pong_set_config` |
| 生命周期 | `ping_pong_start` `ping_pong_stop` `ping_pong_reset` |
| 运行驱动 | `ping_pong_process` `ping_pong_on_tx_done` `ping_pong_on_rx_done` |
| 状态查询 | `ping_pong_get_state` `ping_pong_get_role` `ping_pong_get_stats` `ping_pong_is_valid` |

## 配置

| 参数 | 说明 |
|------|------|
| `max_retries` | 仅 `MASTER` 使用，最大重试次数 |
| `rx_timeout_ms` | 接收超时；`MASTER` 必须大于 0，`SLAVE` 设为 0 表示持续监听 |
| `tx_timeout_ms` | 发送超时；设为 0 表示关闭 TX 超时检测 |

默认值在 `init()` 中填充，必要时再用 `set_config()` 覆盖。

## 通知与报文

| 通知 | 含义 |
|------|------|
| `PING_PONG_NOTIFY_TX_REQUEST` | 请求上层填充发送缓冲区并启动发送 |
| `PING_PONG_NOTIFY_RX_REQUEST` | 请求上层进入接收模式 |
| `PING_PONG_NOTIFY_SUCCESS` | 本轮协议成功 |
| `PING_PONG_NOTIFY_FAIL` | 本轮失败，`fail_reason` 说明原因 |
| `PING_PONG_NOTIFY_RX_TIMEOUT` | 仅 `SLAVE` 可能收到的接收超时通知 |

`FAIL` 的常见原因包括超时、重试耗尽、解析错误、CRC 错误、TX 超时和冲突。

协议包固定为 6 字节：

```text
+--------+---------+---------+----------+---------+---------+
| type   | seq_hi  | seq_lo  | reserved | crc_hi  | crc_lo  |
| 1 byte | 1 byte  | 1 byte  | 1 byte   | 1 byte  | 1 byte  |
+--------+---------+---------+----------+---------+---------+
```

- `type`: `0x01` = `PING`，`0x02` = `PONG`
- `seq`: 16-bit 序列号，大端
- `crc`: CRC-16/CCITT，初值 `0xFFFF`，多项式 `0x1021`

## 集成方式

1. 分配连续实例内存，大小由 `ping_pong_instance_size()` 决定。
2. 实现端口层的 `get_time_ms` 和 `notify`。
3. 调用 `ping_pong_init()` 初始化，再按需调用 `ping_pong_set_config()`。
4. 使用 `ping_pong_start()` 启动 `MASTER` 或 `SLAVE`。
5. 在主循环里周期调用 `ping_pong_process()`。
6. 在底层发送或接收完成后，分别回调 `ping_pong_on_tx_done()` / `ping_pong_on_rx_done()`。

示例请看：

- [主机端示例](../sample/master_example.c)
- [从机端示例](../sample/slave_example.c)
- [单元测试](../test/test_ping_pong.c)
