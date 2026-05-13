# PingPong 中间件设计说明

PingPong 只负责协议核心：状态机、收发时序、Ping/Pong 编解码、超时与重试、冲突检测、设备身份过滤和基础统计。硬件访问、日志、事件分发和业务决策都由上层端口适配承担。

## 一句话

- 这是一个面向嵌入式场景的 Ping-Pong 协议核心模块
- 同一实例可配置为 `MASTER` 或 `SLAVE`
- 通过端口回调注入时间、通知和调试能力
- v2 包格式携带 `network_id`、`src_id`、`dst_id`，用于网络隔离和设备地址过滤

## 边界

- 应由 PingPong 处理：状态切换、Ping / Pong 编解码、超时、重试、冲突检测、identity 过滤、统计
- 不应由 PingPong 处理：具体硬件收发、时间源、日志、事件总线、业务决策、密钥管理和认证

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
| 静态实例 | `PING_PONG_DEFINE_INSTANCE` `ping_pong_instance_size` |
| 初始化与配置 | `ping_pong_init` `ping_pong_set_config` `ping_pong_get_default_config` |
| 生命周期 | `ping_pong_start` `ping_pong_stop` `ping_pong_reset` |
| 运行驱动 | `ping_pong_process` `ping_pong_on_tx_done` `ping_pong_on_rx_done` |
| 状态查询 | `ping_pong_get_state` `ping_pong_get_role` `ping_pong_get_stats` `ping_pong_is_valid` |
| 包构建辅助 | `ping_pong_build_ping` `ping_pong_build_pong` `ping_pong_build_ping_ex` `ping_pong_build_pong_ex` |

## 配置

| 参数 | 说明 |
|------|------|
| `max_retries` | 仅 `MASTER` 使用，最大重试次数 |
| `rx_timeout_ms` | 接收超时；`MASTER` 必须大于 0，`SLAVE` 设为 0 表示持续监听 |
| `tx_timeout_ms` | 发送超时；设为 0 表示关闭 TX 超时检测 |
| `auto_restart` | `MASTER` 成功或失败后是否自动进入下一轮 |
| `restart_delay_ms` | 自动下一轮启动前的延迟 |
| `network_id` | v2 包网络 ID，非匹配网络会被忽略并计入解析错误 |
| `src_id` | 本机设备地址 |
| `dst_id` | 对端设备地址 |

默认值在 `ping_pong_config.h` 中定义，`ping_pong_init()` 会自动填充；必要时可用 `ping_pong_get_default_config()` 读取默认值后局部修改，再用 `ping_pong_set_config()` 覆盖。

## 创建实例

推荐在嵌入式工程中使用静态实例宏，避免手写 buffer 大小和强制类型转换：

```c
PING_PONG_DEFINE_INSTANCE(g_master);

ping_pong_port_t port = {
    .get_time_ms = platform_get_time_ms,
    .notify      = master_notify,
    .user_data   = NULL,
    .trace       = NULL,
};

ping_pong_config_t config;
ping_pong_get_default_config(&config);
config.network_id = 0x01020304u;
config.src_id = 0x1001u;
config.dst_id = 0x2002u;

ping_pong_init(g_master, &port);
ping_pong_set_config(g_master, &config);
```

`PING_PONG_DEFINE_INSTANCE(name)` 会定义：

- 一个静态 backing buffer：`name_buffer`
- 一个对象指针：`ping_pong_t *name`

如果需要自定义内存管理，仍可使用 `ping_pong_instance_size()` 查询所需字节数后自行分配。

## 通知与报文

| 通知 | 含义 |
|------|------|
| `PING_PONG_NOTIFY_TX_REQUEST` | 请求上层启动发送，缓冲区内已是核心编码好的 Ping/Pong 包 |
| `PING_PONG_NOTIFY_RX_REQUEST` | 请求上层进入接收模式 |
| `PING_PONG_NOTIFY_SUCCESS` | 本轮协议成功 |
| `PING_PONG_NOTIFY_FAIL` | 本轮失败，`fail_reason` 说明原因 |

Slave RX 超时由核心内部处理：模块会累计 `rx_timeout_count` 并重新发出 `PING_PONG_NOTIFY_RX_REQUEST`，不会额外发送独立的 RX timeout 通知。

`TX_REQUEST` 中有两个长度字段：

- `tx_buffer_size`：发送缓冲区容量，用于兼容和调试；不应直接作为无线发送长度
- `tx_len`：本次真实应发送长度；当前固定为 `PING_PONG_PACKET_SIZE`，即 24 字节

上层端口应这样发送：

```c
radio_start_tx(n->payload.tx_request.tx_buffer,
               n->payload.tx_request.tx_len);
```

`FAIL` 的常见原因包括超时、重试耗尽、解析错误、CRC 错误、TX 超时和冲突。

v2 协议包固定为 24 字节，前 3 字节保留旧 `type + seq` 前缀，便于调试和过渡：

```text
+------+--------+---------+---------+------------+------------+--------+--------+----------+---------+
| type | seq_hi | seq_lo  | version | network_id | src / dst  | flags  | nonce  | reserved | crc16   |
| 1B   | 1B     | 1B      | 1B      | 4B         | 2B + 2B    | 2B     | 4B     | 6B       | 2B      |
+------+--------+---------+---------+------------+------------+--------+--------+----------+---------+
```

- `type`: `0x01` = `PING`，`0x02` = `PONG`
- `seq`: 16-bit 序列号，大端
- `version`: 当前为 `PING_PONG_PROTOCOL_VERSION_V2`
- `network_id`: 网络 ID，大端
- `src_id` / `dst_id`: 源设备地址与目标设备地址，大端
- `crc16`: CRC-16/CCITT，初值 `0xFFFF`，多项式 `0x1021`

解析端会继续兼容旧 6 字节 v1 包；发送端默认生成 24 字节 v2 包。Phase 6 会在当前 v2 预留字段基础上加入认证与防重放。

## 统计

除基础成功、失败、重试、收发、冲突统计外，还记录：

- `crc_error_count`：CRC 错误次数
- `parse_error_count`：解析错误次数，包括 v2 identity 不匹配
- `rx_timeout_count`：接收超时次数
- `tx_timeout_count`：发送超时次数

## 集成方式

1. 使用 `PING_PONG_DEFINE_INSTANCE(name)` 定义静态实例，或用 `ping_pong_instance_size()` 自行分配连续内存。
2. 实现端口层的 `get_time_ms` 和 `notify`。
3. 调用 `ping_pong_init()` 初始化，再按需调用 `ping_pong_set_config()`。
4. 使用 `ping_pong_start()` 启动 `MASTER` 或 `SLAVE`。
5. 在主循环里周期调用 `ping_pong_process()`。
6. 在底层发送或接收完成后，分别回调 `ping_pong_on_tx_done()` / `ping_pong_on_rx_done()`。

示例请看：

- [主机端示例](../sample/master_example.c)
- [从机端示例](../sample/slave_example.c)
- [单元测试](../test/test_ping_pong.c)
