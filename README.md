# PingPong 中间件设计准则

## 1. 核心设计哲学

| 原则 | 说明 |
|------|------|
| **单一职责** | 只实现 Ping-Pong 协议的核心逻辑，不做任何其他事 |
| **依赖倒置** | 不依赖任何外部模块（事件总线、日志、硬件），外部模块依赖 PingPong 的接口 |
| **零知识原则** | 不知道按键、Radio、事件总线、日志的存在，只知道协议、时间和通知回调 |
| **被动驱动** | 不主动轮询任何硬件，所有输入来自外部调用，所有输出通过回调通知 |

---

## 2. 边界准则

### 2.1 该在 PingPong 内

| 类别 | 具体内容 |
|------|---------|
| 协议逻辑 | 状态机、超时计算、重传逻辑 |
| 编解码 | Ping 包构建、Pong 包解析 |
| 协议参数 | 序列号、重传计数、超时阈值 |
| 统计信息 | 成功/失败计数、RSSI/SNR、RTT |
| 公开 API | init / start / stop / process / on_xxx / get_state / get_stats |

### 2.2 不该在 PingPong 内

| 类别 | 归属 |
|------|------|
| 硬件访问（GPIO/SPI） | HAL / 设备驱动 |
| 事件总线订阅/发布 | 装配层（main.c） |
| 日志输出实现 | 端口层或装配层 |
| 按键/Radio 模块语义 | 各自模块或装配层 |
| 业务决策（什么按键启动） | 装配层 |

---

## 3. 状态机准则

### 3.1 状态定义（只表示阶段，不表示结果）

```
PING_PONG_STATE_IDLE            // 空闲
PING_PONG_STATE_RUNNING_TX      // 运行中：发送阶段
PING_PONG_STATE_RUNNING_RX_WAIT // 运行中：等待接收
PING_PONG_STATE_STOPPED         // 已停止
```

### 3.2 状态准则

- **SUCCESS/FAIL 不作为持久状态**，只作为通知上抛
- 状态转换必须由事件触发（start / stop / tx_done / rx_done / timeout）
- 非法状态转换被忽略，不崩溃
- 状态可查询（`ping_pong_get_state`）

---

## 4. 时间准则

| 要求 | 说明 |
|------|------|
| 时间由外部传入 | 所有 API 带 `now_ms` 参数 |
| 端口层不包含 `get_tick_ms` | 避免重复机制 |
| 超时计算使用传入时间 | `elapsed = now_ms - start_ms` |
| 支持 tick 回绕 | 使用差值比较，而非直接比较 |

---

## 5. 通知准则

### 5.1 单一出口

> 只有一个通知出口：`port.notify()` 回调。

### 5.2 通知类型（定稿 8 类）

| 通知类型 | 说明 |
|---------|------|
| `PING_PONG_NOTIFY_STARTED` | 协议已启动 |
| `PING_PONG_NOTIFY_STOPPED` | 协议已停止 |
| `PING_PONG_NOTIFY_TX_REQUEST` | 请求发送 Ping 包 |
| `PING_PONG_NOTIFY_RX_REQUEST` | 请求进入接收模式（可选） |
| `PING_PONG_NOTIFY_SUCCESS` | Ping-Pong 成功 |
| `PING_PONG_NOTIFY_FAIL` | Ping-Pong 失败 |
| `PING_PONG_NOTIFY_RETRY` | 发生重传 |
| `PING_PONG_NOTIFY_STATS_UPDATED` | 统计更新 |

### 5.3 通知数据准则

- 使用 `union payload` 携带不同类型的数据
- 必须包含 `timestamp_ms` 和 `seq`
- 上层在回调中决定如何分发（事件总线、队列、直接处理）

---

## 6. 端口抽象准则

### 6.1 端口结构体定义

```c
typedef struct ping_pong_port {
    void (*notify)(void *ctx, const ping_pong_notify_t *notify);
} ping_pong_port_t;
```

### 6.2 端口准则

| 要求 | 说明 |
|------|------|
| 极简 | 只有一个 `notify` 回调 |
| 无时间函数 | 时间由外部传入 |
| 无日志函数 | 日志由装配层在回调中处理 |
| 可为 NULL 检查 | 调用前检查 `if (port.notify)` |

---

## 7. 公开 API 准则

### 7.1 必须提供的 API

| API | 说明 |
|-----|------|
| `ping_pong_init` | 初始化，注入配置和端口 |
| `ping_pong_start` | 启动协议 |
| `ping_pong_stop` | 停止协议 |
| `ping_pong_process` | 轮询处理（超时检测） |
| `ping_pong_on_tx_done` | 发送完成通知 |
| `ping_pong_on_rx_done` | 接收完成通知 |
| `ping_pong_on_rx_timeout` | 接收超时通知 |
| `ping_pong_on_rx_error` | 接收错误通知 |
| `ping_pong_get_state` | 获取当前状态 |
| `ping_pong_get_stats` | 获取统计信息 |

### 7.2 API 设计准则

- 第一个参数是 `void *ctx`（调用者分配上下文）
- 时间相关 API 都带 `now_ms` 参数
- 返回值简单（0 成功，-1 失败）
- 不分配动态内存

---

## 8. 资源准则

| 要求 | 说明 |
|------|------|
| 零动态内存 | 上下文由调用者静态分配 |
| 固定缓冲区 | 发送缓冲区大小编译期确定 |
| 无阻塞 | 所有函数立即返回 |
| 不可重入 | 单线程模型，不支持多实例并发 |
| 上下文大小可控 | 建议 ≤ 256 字节 |

---

## 9. 可测试性准则

| 要求 | 说明 |
|------|------|
| 无硬件依赖 | 可在主机上单元测试 |
| 时间可 Mock | `now_ms` 由测试传入 |
| 通知可记录 | `notify` 回调可记录调用序列 |
| 状态可查询 | 提供 `get_state` 接口 |

---

## 10. 错误处理准则

| 场景 | 处理方式 |
|------|---------|
| 未初始化调用 | 忽略或返回默认值 |
| 非法状态转换 | 忽略，不崩溃 |
| 解析失败 | 触发重传或失败通知 |
| 超时 | 自动重传或失败 |
| 接收错误 | 直接失败，不重传 |

---

## 11. 依赖关系

```
main.c (装配层)
  - 实现 notify 回调
  - 调用 ping_pong_xxx API
  - 桥接到事件总线/日志/Radio
        ▲
        │ 依赖
        │
ping_pong.h / ping_pong.c
  - 不依赖任何外部头文件
  - 不调用任何硬件/OS/事件总线 API
  - 只通过 notify 回调输出
```

**依赖方向**：`main.c → ping_pong.c`  
**PingPong 对外部模块的依赖 = 0**

---

## 12. 设计准则一句话总结

> **PingPong 只做协议：状态机 + 编解码 + 超时重传，时间由外部传入，事件通过单一回调通知，对硬件、事件总线、日志、按键、Radio 一无所知。**

---

*文档版本：1.0*  
*最后更新：2026-01-XX*
