/*
 * PingPong 从机端示例
 *
 * 演示 Slave 角色的典型使用流程：
 * - 初始化（默认从机参数）
 * - 若收到 Ping，则响应 Pong
 * - 若未收到 Ping，则持续监听
 *
 * 本示例使用模拟时间和回调驱动，无需真实硬件。
 */

#include "ping_pong.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ==================== 时间模拟 ==================== */

static uint32_t g_time_ms = 0;

static uint32_t mock_get_time_ms(void)
{
    return g_time_ms;
}

/* ==================== CRC-16 辅助 ==================== */

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t j;
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void build_ping_packet(uint8_t *buf, uint16_t seq)
{
    buf[0] = 0x01; /* type = PING */
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    uint16_t crc = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

static void build_pong_packet(uint8_t *buf, uint16_t seq)
{
    buf[0] = 0x02; /* type = PONG */
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    uint16_t crc = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== 模拟发送通道 ==================== */

static uint8_t g_tx_buf[128];
static uint32_t g_tx_len;
static int g_tx_done_pending;

static void mock_radio_send(const uint8_t *data, uint32_t len)
{
    if (len > sizeof(g_tx_buf)) {
        len = sizeof(g_tx_buf);
    }
    memcpy(g_tx_buf, data, len);
    g_tx_len = len;
    g_tx_done_pending = 1;
}

/* ==================== Slave 实例 ==================== */

static uint8_t g_slave_mem[512];
#define g_slave ((ping_pong_t *)g_slave_mem)

/* 状态标志 */
static int g_rx_listening;

/* ==================== 回调实现 ==================== */

static void slave_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                         void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            /* 构建 Pong 包并模拟发送 */
            build_pong_packet(notify->payload.tx_request.tx_buffer,
                              (uint16_t)notify->seq);
            mock_radio_send(notify->payload.tx_request.tx_buffer,
                            notify->payload.tx_request.tx_buffer_size);
            printf("  [Slave] TX_REQUEST: 发送 Pong, seq=%u\n", notify->seq);
            break;

        case PING_PONG_NOTIFY_RX_REQUEST:
            g_rx_listening = 1;
            printf("  [Slave] RX_REQUEST: 进入监听模式, seq=%u\n", notify->seq);
            break;

        case PING_PONG_NOTIFY_RX_PING:
            printf("  [Slave] RX_PING: 收到 Ping! seq=%u, RSSI=%d, SNR=%d\n",
                   notify->seq,
                   notify->payload.rx_ping.rssi,
                   notify->payload.rx_ping.snr);
            break;

        case PING_PONG_NOTIFY_RX_TIMEOUT:
            printf("  [Slave] RX_TIMEOUT: 监听超时, 继续等待...\n");
            break;

        default:
            break;
    }
}

/* ==================== 场景模拟 ==================== */

/*
 * 场景 1：正常接收 Ping 并响应 Pong
 *   Slave 监听 → 收到 Ping → 回复 Pong → TX 完成 → 回到监听
 */
static void scenario_receive_and_respond(uint32_t base_time, uint16_t seq)
{
    printf("\n--- 场景: 收到 Ping 并响应 Pong (seq=%u) ---\n", seq);

    /* 模拟收到来自 Master 的 Ping */
    uint8_t ping[6];
    build_ping_packet(ping, seq);
    g_time_ms = base_time;
    g_tx_done_pending = 0;
    ping_pong_on_rx_done(g_slave, ping, 6, -48, 11);

    /* Pong 发送完成 → 回到 RX_WAIT 监听 */
    g_time_ms = base_time + 10;
    if (g_tx_done_pending) {
        g_tx_done_pending = 0;
        ping_pong_on_tx_done(g_slave);
    }
}

/*
 * 场景 2：持续监听，未收到 Ping
 *   Slave 持续 process → 无超时（slave_rx_timeout_ms=0）→ 保持监听
 */
static void scenario_keep_listening(uint32_t base_time)
{
    uint32_t i;
    printf("\n--- 场景: 持续监听（无 Ping） ---\n");

    /* 模拟多次 process 调用，Slave 保持在 RX_WAIT */
    for (i = 0; i < 5; i++) {
        g_time_ms = base_time + i * 1000;
        ping_pong_process(g_slave);
    }

    ping_pong_state_t state = ping_pong_get_state(g_slave);
    printf("  [Slave] 经过 %u 次 process 后状态: %s\n",
           (unsigned)i,
           (state == PING_PONG_STATE_RX_WAIT) ? "RX_WAIT (持续监听)" : "其他");
}

/*
 * 场景 3：带超时的持续监听
 *   设置 slave_rx_timeout_ms 后超时重新进入监听
 *   （本示例中 slave_rx_timeout_ms=0，不会超时，此场景仅做说明）
 */

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== PingPong 从机端示例 ===\n");
    printf("配置: 默认从机参数, slave_rx_timeout_ms=0 (永不超时)\n");

    /* 初始化 */
    memset(g_slave_mem, 0, sizeof(g_slave_mem));

    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = slave_notify,
        .user_data = NULL,
        .trace = NULL,
    };

    ping_pong_config_t config = {
        .timeout_ms = 3000,          /* Master 超时参数（Slave 不使用） */
        .max_retries = 3,            /* Master 重传参数（Slave 不使用） */
        .tx_buffer_size = 64,        /* 发送缓冲区（Pong 回复需要） */
        .slave_rx_timeout_ms = 0,    /* 0=永不超时，持续监听 */
        .tx_timeout_ms = 0,          /* 不启用 TX 超时保护 */
    };

    assert(ping_pong_init(g_slave, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(g_slave, &config) == PING_PONG_OK);

    /* 启动 Slave，进入监听模式 */
    g_time_ms = 0;
    ping_pong_start(g_slave, PING_PONG_ROLE_SLAVE);

    /*
     * 场景 1: 收到 Ping 并响应 Pong (seq=0)
     * 监听 → 收到 Ping → 回复 Pong → 回到监听
     */
    scenario_receive_and_respond(100, 0);

    /*
     * 场景 2: 持续监听，无 Ping 到来
     * 多次 process → 保持 RX_WAIT 状态
     */
    scenario_keep_listening(1000);

    /*
     * 场景 3: 再次收到 Ping 并响应 (seq=1)
     * 验证 Slave 可连续处理多次 Ping
     */
    scenario_receive_and_respond(6000, 1);

    /*
     * 场景 4: 第三次收到 Ping (seq=2)
     */
    scenario_receive_and_respond(8000, 2);

    /* 打印最终统计 */
    ping_pong_stats_t stats;
    ping_pong_get_stats(g_slave, &stats);
    printf("\n========== 最终统计 ==========\n");
    printf("收到 Ping 次数: %u\n", stats.slave_rx_count);
    printf("冲突次数: %u\n", stats.conflict_count);

    printf("\n=== 从机端示例完成 ===\n");
    return 0;
}
