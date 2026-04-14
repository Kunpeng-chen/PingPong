# PingPong

一个面向嵌入式场景的轻量级 Ping-Pong 协议模块，当前仓库聚焦于“主从一体”的协议核心实现。

它只负责协议状态机、收发时序、超时重试、基础统计与通知回调，不直接依赖 Radio、日志、事件总线或具体硬件驱动。

## 特性

- 单文件接口清晰：核心代码集中在 `src/ping_pong.h` 与 `src/ping_pong.c`
- 零外部模块依赖：通过端口层注入时间与通知能力
- 支持 Master / Slave 两种角色
- 支持超时、重试、冲突检测与基础统计
- 适合被上层应用或板级适配层集成到现有工程

## 仓库结构

```text
.
|- src/
|  |- ping_pong.h       # 对外 API、配置、通知和类型定义
|  |- ping_pong.c       # 协议状态机与核心逻辑实现
|  `- README.md         # 详细设计原则与边界说明
|- sample/
|  |- master_example.c  # Master 角色示例
|  `- slave_example.c   # Slave 角色示例
`- test/
   `- test_ping_pong.c  # 单元测试
```

## 模块边界

PingPong 模块内部负责：

- Ping / Pong 报文头处理
- 协议状态切换
- Master 超时与重试
- Slave 等待与超时通知
- 成功、失败、冲突与统计上报

PingPong 模块外部负责：

- 时间基准获取
- 实际无线收发
- 日志打印与业务决策
- 事件分发与系统调度
- 上下文与发送缓冲区分配

## 快速接入

1. 在上层提供 `get_time_ms` 与 `notify` 两个端口函数。
2. 为 `ping_pong_t` 上下文和发送缓冲区预留连续内存。
3. 调用 `ping_pong_init()` 初始化实例。
4. 调用 `ping_pong_set_config()` 设置超时、重试次数和缓冲区大小。
5. 通过 `ping_pong_start()` 以 `MASTER` 或 `SLAVE` 角色启动。
6. 在主循环中持续调用 `ping_pong_process()` 检查超时。
7. 在底层驱动完成发送或接收后，分别回调 `ping_pong_on_tx_done()`、`ping_pong_on_rx_done()`。

## 运行流程

- Master 启动后先请求发送 Ping，发送完成后进入等待 Pong 状态。
- 若在超时时间内收到合法 Pong，则上报成功并更新 RTT / RSSI / SNR。
- 若超时且未达到最大重试次数，则上报重试并重新发起发送。
- 若达到最大重试次数仍失败，则上报失败。
- Slave 启动后进入等待 Ping 状态，收到 Ping 后请求发送 Pong。
- 若角色收到与预期不符的报文类型，则上报冲突。

## 核心接口

- 内存计算：`ping_pong_instance_size`
- 生命周期：`ping_pong_init` `ping_pong_set_config` `ping_pong_start` `ping_pong_stop` `ping_pong_reset`
- 轮询与事件：`ping_pong_process` `ping_pong_on_tx_done` `ping_pong_on_rx_done`
- 状态查询：`ping_pong_get_state` `ping_pong_get_role` `ping_pong_get_stats` `ping_pong_is_valid`

主要通知类型包括：

- `PING_PONG_NOTIFY_TX_REQUEST`
- `PING_PONG_NOTIFY_RX_REQUEST`
- `PING_PONG_NOTIFY_SUCCESS`
- `PING_PONG_NOTIFY_FAIL`（含 `fail_reason`，涵盖超时/重试耗尽/解析错误/CRC 错误/TX 超时/冲突）
- `PING_PONG_NOTIFY_RX_TIMEOUT`

## 适用场景

- 点对点链路连通性验证
- 无线模块基础收发联调
- 嵌入式协议状态机演示与裁剪复用
- 需要与现有 HAL / Radio / Event Bus 解耦的场景

## 说明

- 当前仓库更像协议核心原型，重点是接口边界和状态机逻辑。
- 更细的设计约束可查看 `src/README.md`。
