# PingPong 正式生产部署计划

## 1. 目标

本文档用于指导 PingPong 从“工程验证可用”演进到“正式生产部署可用”。

当前 PingPong 已适合单主单从链路健康检查、样机验证和小规模试点。正式生产部署的目标是：

- 在真实弱信号、干扰和偶发坏包环境下稳定运行
- 支持明确的设备身份与网络隔离
- 降低误失败率
- 提升长期运行可观测性
- 恢复并保持完整测试体系
- 为后续安全认证、协议扩展和产测闭环留出空间

## 2. 当前基线

当前 main 已具备：

- C11 + CMake 构建
- gcc / clang CI
- 覆盖率门禁
- 主从角色状态机
- 核心侧 Ping/Pong 包构建
- 固定 6 字节包格式
- 明确的 `tx_len`
- 静态实例宏 `PING_PONG_DEFINE_INSTANCE(name)`
- CRC-16/CCITT 校验
- RX / TX 超时处理
- Master 重试
- 基础统计与错误统计

当前主要限制：

- Master 收到 CRC 或 parse 错误后会立即失败
- 无设备地址与网络 ID
- 无认证、防伪造、防重放能力
- 包格式扩展能力有限
- 旧大测试文件尚未迁移到最新通知模型
- 未完成真实硬件长时间压力测试

## 3. 生产部署准入标准

正式生产部署前，建议至少满足以下准入条件。

### 3.1 协议鲁棒性

- CRC 错误不应立即导致 Master fail
- 非当前 seq 的包应可忽略并统计
- 未知 type 包应可忽略并统计
- 冲突包应有明确策略
- 超时与重试行为可配置且可观测

### 3.2 设备身份

- 包内包含 `network_id`
- 包内包含 `src_id`
- 包内包含 `dst_id`
- 设备只处理目标网络和目标地址的包
- 广播地址策略明确

### 3.3 可观测性

- 统计字段覆盖成功、失败、重试、CRC、parse、冲突、RX timeout、TX timeout
- 可查询最近失败原因
- 可查询最近收包信息，如 RSSI、SNR、RTT、seq
- 可选 trace 能够定位状态迁移

### 3.4 测试

- 恢复旧测试文件或完成等价迁移
- 单元测试覆盖率保持不低于 90%
- 覆盖 Master 忽略坏包后继续等待的行为
- 覆盖地址过滤行为
- 覆盖网络 ID 过滤行为
- 覆盖序列号回绕
- 覆盖长时间 process 调用稳定性

### 3.5 硬件验证

- 24 小时连续运行无死锁、无状态卡死
- 72 小时弱信号或干扰环境运行记录完整
- 连续失败后 radio reset / protocol reset 策略验证通过
- 不同超时和重试参数组合验证通过

## 4. 分阶段计划

## Phase 1：恢复测试体系与 CI 稳定性

目标：让工程质量基线稳定下来。

任务：

1. 迁移 `test/test_ping_pong.c` 中历史 `PING_PONG_NOTIFY_RX_TIMEOUT` 相关断言。
2. 恢复 `test_ping_pong` 到 CMake。
3. 保留 `test_ping_pong_extensions` 作为新增行为专项测试。
4. 确认 gcc / clang / coverage 全部通过。
5. 清理 PR 描述或文档中关于旧测试临时排除的说明。

验收标准：

- `ctest --output-on-failure` 全部通过
- coverage >= 90%
- 无临时排除测试目标

建议优先级：最高。

## Phase 2：增强 Master 对坏包的容错

目标：降低真实无线环境下的误失败。

当前行为：

- Master 收到 CRC 错误 Pong 后立即 FAIL
- Master 收到 seq 不匹配 Pong 后立即 FAIL
- Master 收到未知 type 后立即 FAIL

建议行为：

- CRC 错误：`crc_error_count++`，继续等待 RX timeout
- seq 不匹配：`parse_error_count++`，继续等待 RX timeout
- 未知 type：`parse_error_count++`，继续等待 RX timeout
- Ping 冲突：是否立即 FAIL 需要讨论，可以配置

新增配置建议：

```c
bool fail_on_conflict;
bool fail_on_parse_error;
bool fail_on_crc_error;
```

或保持轻量：默认忽略 CRC/parse 错误，只对冲突 fail。

验收标准：

- 单个坏包不会导致 Master 立即失败
- Master 最终仍由 RX timeout / retry 机制收敛
- 统计字段准确累计
- 单元测试覆盖坏包后成功收到合法 Pong 的路径

建议优先级：最高。

## Phase 3：引入设备地址和网络 ID

目标：避免多设备环境中误响应、串包和跨网络干扰。

建议包格式从当前 6 字节扩展为版本化格式：

```text
magic | version | type | flags | seq_hi | seq_lo | network_id | src_id | dst_id | crc_hi | crc_lo
```

可选更紧凑格式：

```text
version | type | seq_hi | seq_lo | network_id | src_id | dst_id | flags | crc_hi | crc_lo
```

字段建议：

- `version`：协议版本
- `type`：PING / PONG
- `seq`：16-bit 序列号
- `network_id`：网络或产品线隔离
- `src_id`：发送方设备 ID
- `dst_id`：目标设备 ID
- `flags`：广播、确认、保留扩展位
- `crc`：CRC-16/CCITT

API 建议：

```c
typedef struct {
    uint16_t local_id;
    uint16_t peer_id;
    uint16_t network_id;
    uint8_t allow_broadcast;
} ping_pong_identity_t;

ping_pong_err_t ping_pong_set_identity(ping_pong_t *pp,
                                       const ping_pong_identity_t *identity);
```

验收标准：

- 非本网络包被忽略并统计
- 非本设备目标包被忽略并统计
- 广播策略明确
- Master 和 Slave 均覆盖地址过滤测试

建议优先级：高。

## Phase 4：失败恢复策略

目标：在真实现场中避免长期卡死或连续失败不可恢复。

建议新增上层策略文档，而不一定全部放入核心。

建议策略：

- 连续 fail N 次：重置 PingPong 实例
- 连续 fail M 次：重置 radio
- 连续 fail K 次：切换信道或重新配置射频参数
- 长时间无成功：上报设备健康状态

核心可新增辅助统计：

```c
uint32_t consecutive_fail_count;
uint32_t last_fail_reason;
uint32_t last_fail_timestamp_ms;
```

验收标准：

- 连续失败可被上层准确识别
- 示例中体现 reset / radio reset 策略
- 文档中给出推荐阈值

建议优先级：中高。

## Phase 5：安全与防重放

目标：在有攻击风险或跨设备干扰风险的场景中提高可信度。

最小安全增强：

- 增加 rolling counter 或 nonce
- 增加轻量 MAC 字段
- 不接受过旧 seq 或重复 counter

可选方案：

- SipHash / CMAC / HMAC 的截断输出
- 设备预共享 key
- network_id + device_id + seq 参与认证

注意：

- CRC 不提供安全性，只能防误码
- 安全能力可能增加包长和计算成本
- 是否做安全增强取决于产品场景

验收标准：

- 伪造包无法通过认证
- 重放旧包无法通过认证
- 安全关闭时仍可保持轻量测试模式

建议优先级：视场景决定。

## Phase 6：硬件压力测试与现场试点

目标：验证真实物理链路中的稳定性。

测试建议：

1. 近距离强信号 24 小时稳定性测试
2. 中距离普通信号 24 小时测试
3. 弱信号 / 边缘距离 72 小时测试
4. 干扰环境测试
5. 高丢包率模拟测试
6. radio reset 恢复测试
7. 主从断电重启恢复测试
8. seq 回绕测试

记录指标：

- success_count
- fail_count
- retry_count
- crc_error_count
- parse_error_count
- conflict_count
- rx_timeout_count
- tx_timeout_count
- RTT 分布
- RSSI / SNR 分布
- 连续失败最大次数

验收标准建议：

- 24 小时强信号无连续不可恢复失败
- 72 小时弱信号无状态卡死
- 所有失败均有可解释统计
- 断电重启后可自动恢复通信

## 5. 建议里程碑

### M1：测试恢复与 CI 稳定

- 迁移旧测试
- 恢复完整 CMake 测试目标
- coverage >= 90%

### M2：无线鲁棒性增强

- Master 忽略 CRC / parse 坏包
- 单元测试覆盖坏包后继续等待
- 文档更新错误处理策略

### M3：多设备可用

- 增加 network_id / src_id / dst_id
- 地址过滤测试完成
- 示例更新

### M4：试点可观测

- 增加 last_fail_reason / consecutive_fail_count
- 示例增加 reset 策略
- 日志与统计字段文档完善

### M5：生产试点

- 24h / 72h 硬件测试完成
- 形成测试报告
- 小规模现场试点

## 6. 需要讨论的决策点

1. 是否需要多设备地址？如果只做一主一从，可以延后。
2. 地址字段宽度用 8-bit 还是 16-bit？
3. `network_id` 是否必须？
4. Master 收到 Ping 冲突时，是立即 fail，还是忽略并继续等待？
5. 是否需要认证 / 防重放？
6. 包格式是否现在就升级为 versioned packet？
7. 是否接受破坏兼容，还是保留旧 6 字节包格式作为 v1？
8. 生产部署目标是“链路探测”还是“设备在线心跳”？
9. 是否需要上报统计到业务层？
10. 是否需要提供 radio reset 示例？

## 7. 推荐下一步

建议下一步先做两个 PR：

1. `test: migrate legacy tests to current notification model`
2. `feat: make master tolerate corrupted/unrelated packets until timeout`

这两个完成后，再讨论是否引入地址和版本化包格式。
