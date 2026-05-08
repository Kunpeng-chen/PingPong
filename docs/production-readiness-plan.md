# PingPong 正式生产部署计划

## 1. 目标

本文档用于指导 PingPong 从“工程验证可用”演进到“正式生产部署可用”。

当前 PingPong 已适合单主单从链路健康检查、样机验证和小规模试点。正式生产部署的目标是：

- 在真实弱信号、干扰和偶发坏包环境下稳定运行
- 即使当前只是一主一从，也支持明确的设备身份与网络隔离
- 降低误失败率
- 提升长期运行可观测性
- 恢复并保持完整测试体系
- 引入认证与防重放能力
- 为后续协议扩展、产测闭环和现场诊断留出空间

## 2. 已确认设计决策

以下决策作为正式生产部署方向的输入，不再作为待讨论项。

1. 当前业务场景是一主一从，但仍需要引入设备地址和 `network_id`。
2. `src_id` / `dst_id` 使用 8-bit 字段。
3. `network_id` 使用 8-bit 字段。
4. v2 包长可以接受 24 字节。
5. MAC 使用 SipHash-2-4，并输出 64-bit 认证码。
6. v2 不兼容旧 6 字节 v1 包格式。
7. Phase 1 完成后，先基于当时稳定版本创建 Git tag `v0.1.0`，再开始 Phase 2 及后续破坏性协议升级。
8. Master 收到 CRC 错误、seq 不匹配、未知 type 等坏包时，应统计并继续等待，直到 RX timeout 再触发重试或失败。
9. 正式生产目标需要认证与防重放能力，不能只依赖 CRC。
10. 广播地址默认关闭。
11. Ping 冲突只统计，不立即失败。
12. key 采用运行期配置 API，由上层从配置区或出厂写入区读取后传入 PingPong。
13. counter 第一版暂不做掉电保存；后续可扩展为上层持久化或块分配策略。

这些决策意味着 PingPong 将从“最小链路探测协议”升级为“可生产部署的单主单从安全心跳 / 链路健康协议”。

## 3. 当前基线

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

## 4. 生产部署准入标准

正式生产部署前，建议至少满足以下准入条件。

### 4.1 协议鲁棒性

- CRC 错误不应立即导致 Master fail
- 非当前 seq 的包应可忽略并统计
- 未知 type 包应可忽略并统计
- 非本网络、非本设备目标包应可忽略并统计
- 认证失败包应可忽略并统计
- 重放包应可忽略并统计
- Ping 冲突只统计，不立即失败
- 超时与重试行为可配置且可观测

### 4.2 设备身份

- 包内包含 8-bit `network_id`
- 包内包含 8-bit `src_id`
- 包内包含 8-bit `dst_id`
- 设备只处理目标网络和目标地址的包
- 即使当前一主一从，也不省略身份字段
- 广播地址默认关闭

### 4.3 认证与防重放

- 包内包含 SipHash-2-4 生成的 64-bit MAC
- 认证输入至少包含 version、type、flags、seq、network_id、src_id、dst_id、counter / nonce
- 包内包含 rolling counter 或 nonce
- 第一版 counter 不掉电保存；重启后的强防重放作为后续增强项
- 重复 counter / 旧 counter 包应被拒绝并统计
- CRC 继续用于误码检测，但不作为安全机制

### 4.4 可观测性

- 统计字段覆盖成功、失败、重试、CRC、parse、冲突、RX timeout、TX timeout
- 新增地址过滤、认证失败、重放拒绝相关统计
- 可查询最近失败原因
- 可查询最近收包信息，如 RSSI、SNR、RTT、seq、src_id、dst_id
- 可选 trace 能够定位状态迁移

### 4.5 测试

- 恢复旧测试文件或完成等价迁移
- 单元测试覆盖率保持不低于 90%
- 覆盖 Master 忽略坏包后继续等待的行为
- 覆盖地址过滤行为
- 覆盖网络 ID 过滤行为
- 覆盖认证失败行为
- 覆盖重放拒绝行为
- 覆盖序列号回绕
- 覆盖长时间 process 调用稳定性

### 4.6 硬件验证

- 24 小时连续运行无死锁、无状态卡死
- 72 小时弱信号或干扰环境运行记录完整
- 连续失败后 radio reset / protocol reset 策略验证通过
- 不同超时和重试参数组合验证通过

## 5. 分阶段计划

## Phase 1：恢复测试体系与 CI 稳定性

目标：让工程质量基线稳定下来，并在破坏性协议升级前沉淀一个稳定基线版本。

任务：

1. 迁移 `test/test_ping_pong.c` 中历史 `PING_PONG_NOTIFY_RX_TIMEOUT` 相关断言。
2. 恢复 `test_ping_pong` 到 CMake。
3. 保留 `test_ping_pong_extensions` 作为新增行为专项测试。
4. 确认 gcc / clang / coverage 全部通过。
5. 清理 PR 描述或文档中关于旧测试临时排除的说明。
6. 基于 Phase 1 完成后的 `main` 创建 tag `v0.1.0`。

验收标准：

- `ctest --output-on-failure` 全部通过
- coverage >= 90%
- 无临时排除测试目标
- `v0.1.0` tag 已创建并推送

建议优先级：最高。

## Phase 2：增强 Master 对坏包的容错

目标：降低真实无线环境下的误失败。

当前行为：

- Master 收到 CRC 错误 Pong 后立即 FAIL
- Master 收到 seq 不匹配 Pong 后立即 FAIL
- Master 收到未知 type 后立即 FAIL

目标行为：

- CRC 错误：`crc_error_count++`，继续等待 RX timeout
- seq 不匹配：`parse_error_count++`，继续等待 RX timeout
- 未知 type：`parse_error_count++`，继续等待 RX timeout
- 非本网络 / 非本地址：统计并继续等待
- 认证失败 / 重放包：统计并继续等待
- Ping 冲突：只统计并继续等待，不立即失败

配置建议：

```c
typedef struct {
    uint8_t fail_on_conflict;
    uint8_t fail_on_parse_error;
    uint8_t fail_on_crc_error;
    uint8_t fail_on_auth_error;
} ping_pong_error_policy_t;
```

默认生产策略建议：

```text
fail_on_conflict = false
fail_on_parse_error = false
fail_on_crc_error = false
fail_on_auth_error = false
```

也就是：坏包只统计，不立即失败；超时和重试机制负责收敛。

验收标准：

- 单个坏包不会导致 Master 立即失败
- Master 最终仍由 RX timeout / retry 机制收敛
- 统计字段准确累计
- 单元测试覆盖坏包后成功收到合法 Pong 的路径

建议优先级：最高。

## Phase 3：引入设备地址和网络 ID

目标：即使当前只是一主一从，也通过身份字段避免误响应、串包和跨网络干扰。

v2 不兼容旧 6 字节 v1 包格式。旧格式在 `v0.1.0` 中冻结；v2 之后统一使用版本化生产包格式。

建议生产 v2 包格式：

```text
magic_hi | magic_lo | version | type | flags | seq_hi | seq_lo |
network_id | src_id | dst_id |
counter_3 | counter_2 | counter_1 | counter_0 |
mac_7 | mac_6 | mac_5 | mac_4 | mac_3 | mac_2 | mac_1 | mac_0 |
crc_hi | crc_lo
```

当前 v2 包长为 24 字节，已确认可接受。

字段建议：

- `magic`：协议识别，2 字节
- `version`：协议版本，当前 v2
- `type`：PING / PONG
- `flags`：广播、确认、保留扩展位
- `seq`：16-bit 序列号，用于请求 / 响应匹配
- `network_id`：8-bit 网络或产品线隔离
- `src_id`：8-bit 发送方设备 ID
- `dst_id`：8-bit 目标设备 ID
- `counter`：32-bit rolling counter，用于防重放
- `mac`：SipHash-2-4 64-bit 认证码
- `crc`：CRC-16/CCITT，用于误码检测

API 建议：

```c
typedef struct {
    uint8_t local_id;
    uint8_t peer_id;
    uint8_t network_id;
    uint8_t allow_broadcast;
} ping_pong_identity_t;

ping_pong_err_t ping_pong_set_identity(ping_pong_t *pp,
                                       const ping_pong_identity_t *identity);
```

默认建议：

```text
allow_broadcast = 0
```

验收标准：

- 非本网络包被忽略并统计
- 非本设备目标包被忽略并统计
- 广播默认关闭
- Master 和 Slave 均覆盖地址过滤测试

建议优先级：高。

## Phase 4：认证与防重放

目标：防止伪造 Ping/Pong 和重放旧包。

### 4.1 MAC 的作用

MAC 是 Message Authentication Code，消息认证码。它的作用不是加密内容，而是证明“这个包确实由持有密钥的合法设备生成，并且包内容没有被篡改”。

CRC 只能发现传输误码，不能阻止别人伪造包。攻击者可以自己构造一个包并重新计算 CRC。MAC 则需要共享密钥；没有密钥的人即使知道包格式，也很难伪造出合法 MAC。

在 PingPong 中，MAC 主要解决三件事：

1. 防伪造：别人不能随便伪造一个合法 Ping 或 Pong。
2. 防篡改：包里的 type、seq、src_id、dst_id、counter 等字段被改动后，MAC 校验会失败。
3. 配合 counter 防重放：旧包再次发送时，即使 MAC 正确，也会因为 counter 太旧而被拒绝。

### 4.2 key 的配置方式

key 是 SipHash-2-4 计算 MAC 时使用的共享密钥。Master 和 Slave 必须使用同一个 key，才能互相验证对方发来的包。

已确认第一版采用运行期配置 API：上层从配置区或出厂写入区读取 key，然后调用 `ping_pong_set_security()` 传入 PingPong。

不采用编译期固定 key 作为生产方案，因为它泄露风险高，也不利于量产。

### 4.3 建议安全层

```c
typedef struct {
    const uint8_t *key;
    uint32_t key_len;
    uint32_t tx_counter;
    uint32_t rx_counter_window;
} ping_pong_security_t;

ping_pong_err_t ping_pong_set_security(ping_pong_t *pp,
                                       const ping_pong_security_t *security);
```

认证输入建议包括：

```text
magic, version, type, flags, seq, network_id, src_id, dst_id, counter
```

MAC 算法：

```text
SipHash-2-4，输出 64-bit MAC
```

### 4.4 counter 是否掉电保存

counter 用于防重放。区别在于设备重启后是否记住之前已经用过的 counter。

已确认第一版暂不做 counter 掉电保存：

- 设备重启后 counter 可从运行期配置的初始值重新开始
- 实现简单，不需要写 Flash
- 风险是：攻击者如果录下重启前的旧包，设备重启后可能再次接受这些旧包
- 后续生产增强可再引入上层持久化或块分配策略

第一版仍需在单次上电运行期间拒绝重复 counter 或旧 counter。

防重放策略：

- 每个方向维护独立 counter
- 接收端记录最近接受的最大 counter
- counter 小于或等于已接受值时拒绝
- PingPong 一主一从且不要求乱序接收，默认不需要滑动窗口

新增统计建议：

```c
uint32_t auth_error_count;
uint32_t replay_error_count;
uint32_t address_filter_count;
uint32_t network_filter_count;
```

验收标准：

- 无 key 时不能启用安全模式
- MAC 错误包被忽略并统计
- 单次上电期间重放旧包被忽略并统计
- 合法 counter 单调递增包可通过
- 安全关闭时仍可用于实验室轻量测试

建议优先级：高。

## Phase 5：失败恢复策略

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
9. 认证失败注入测试
10. 重放包注入测试
11. 跨 network_id 干扰测试
12. 错误 dst_id 干扰测试

记录指标：

- success_count
- fail_count
- retry_count
- crc_error_count
- parse_error_count
- conflict_count
- rx_timeout_count
- tx_timeout_count
- auth_error_count
- replay_error_count
- address_filter_count
- network_filter_count
- RTT 分布
- RSSI / SNR 分布
- 连续失败最大次数

验收标准建议：

- 24 小时强信号无连续不可恢复失败
- 72 小时弱信号无状态卡死
- 所有失败均有可解释统计
- 断电重启后可自动恢复通信
- 伪造包、重放包、跨网络包不会触发错误成功

## 6. 建议里程碑

### M1：测试恢复与 CI 稳定

- 迁移旧测试
- 恢复完整 CMake 测试目标
- coverage >= 90%
- 创建并推送 tag `v0.1.0`

### M2：无线鲁棒性增强

- Master 忽略 CRC / parse 坏包
- 单元测试覆盖坏包后继续等待
- 文档更新错误处理策略

### M3：身份化协议 v2

- 增加 `network_id` / `src_id` / `dst_id`
- 引入 versioned packet
- 明确不兼容 v1
- 地址过滤测试完成
- 示例更新

### M4：安全与防重放

- 增加 counter
- 增加 SipHash-2-4 64-bit MAC
- 增加认证失败和重放统计
- 完成伪造 / 重放测试

### M5：试点可观测

- 增加 last_fail_reason / consecutive_fail_count
- 示例增加 reset 策略
- 日志与统计字段文档完善

### M6：生产试点

- 24h / 72h 硬件测试完成
- 形成测试报告
- 小规模现场试点

## 7. 仍需讨论的决策点

当前关键生产设计点已完成初步决策。后续实现时如遇硬件限制，再重新评估 key 存储和 counter 持久化策略。

## 8. 推荐下一步

建议下一步拆成 5 个 PR：

1. `test: migrate legacy tests to current notification model`
2. `release: tag v0.1.0 baseline after test restoration`
3. `feat: make master tolerate corrupted/unrelated packets until timeout`
4. `feat: add identity fields and versioned v2 packet format`
5. `feat: add SipHash-2-4 authentication and replay protection`

建议顺序不要跳过前两项。先恢复测试并打 `v0.1.0`，再做坏包容错、包格式升级和安全能力。
