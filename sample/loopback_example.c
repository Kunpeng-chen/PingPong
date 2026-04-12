/*
 * PingPong 单机自环回示例
 *
 * 演示在同一进程中运行 Master 和 Slave 实例，
 * 通过直接函数调用模拟无线收发，验证协议流程。
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

/* ==================== 环回数据通道 ==================== */

/* 简单环回缓冲区 */
static uint8_t g_loopback_buf[128];
static uint32_t g_loopback_len;
static int g_loopback_pending;

static void loopback_send(const uint8_t *data, uint32_t len)
{
    if (len > sizeof(g_loopback_buf)) {
        len = sizeof(g_loopback_buf);
    }
    memcpy(g_loopback_buf, data, len);
    g_loopback_len = len;
    g_loopback_pending = 1;
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

static void build_packet(uint8_t *buf, uint8_t type, uint16_t seq)
{
    buf[0] = type;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    uint16_t crc = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== Master 和 Slave 实例 ==================== */

static uint8_t g_master_mem[512];
static uint8_t g_slave_mem[512];
#define g_master ((ping_pong_t *)g_master_mem)
#define g_slave  ((ping_pong_t *)g_slave_mem)

static int g_master_tx_done_pending;
static int g_slave_tx_done_pending;

/* Master 回调 */
static void master_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                          void *user_data)
{
    (void)pp;
    (void)user_data;
    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            build_packet(notify->payload.tx_request.tx_buffer, 0x01, (uint16_t)notify->seq);
            loopback_send(notify->payload.tx_request.tx_buffer, 6);
            g_master_tx_done_pending = 1;
            break;
        case PING_PONG_NOTIFY_SUCCESS:
            printf("  [Master] SUCCESS! RTT=%u ms\n", notify->payload.success.rtt_ms);
            break;
        case PING_PONG_NOTIFY_FAIL:
            printf("  [Master] FAIL reason=%u\n", notify->payload.fail.fail_reason);
            break;
        default:
            break;
    }
}

/* Slave 回调 */
static void slave_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                         void *user_data)
{
    (void)pp;
    (void)user_data;
    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            build_packet(notify->payload.tx_request.tx_buffer, 0x02, (uint16_t)notify->seq);
            loopback_send(notify->payload.tx_request.tx_buffer, 6);
            g_slave_tx_done_pending = 1;
            break;
        case PING_PONG_NOTIFY_PING_RECEIVED:
            printf("  [Slave] Ping received, seq=%u\n", notify->payload.ping_received.seq);
            break;
        default:
            break;
    }
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== PingPong Loopback Example ===\n\n");

    ping_pong_port_t master_port = {
        .get_time_ms = mock_get_time_ms,
        .notify = master_notify,
        .user_data = NULL,
        .trace = NULL,
    };
    ping_pong_port_t slave_port = {
        .get_time_ms = mock_get_time_ms,
        .notify = slave_notify,
        .user_data = NULL,
        .trace = NULL,
    };

    ping_pong_config_t config = {
        .timeout_ms = 100,
        .max_retries = 3,
        .tx_buffer_size = 64,
        .slave_rx_timeout_ms = 5000,
        .tx_timeout_ms = 0,
    };

    /* 初始化 */
    memset(g_master_mem, 0, sizeof(g_master_mem));
    memset(g_slave_mem, 0, sizeof(g_slave_mem));
    assert(ping_pong_init(g_master, &master_port) == PING_PONG_OK);
    assert(ping_pong_init(g_slave, &slave_port) == PING_PONG_OK);
    assert(ping_pong_set_config(g_master, &config) == PING_PONG_OK);
    assert(ping_pong_set_config(g_slave, &config) == PING_PONG_OK);

    /* 运行 3 轮 */
    int round;
    for (round = 0; round < 3; round++) {
        printf("--- Round %d ---\n", round + 1);
        g_loopback_pending = 0;
        g_master_tx_done_pending = 0;
        g_slave_tx_done_pending = 0;

        /* 启动 Slave 等待，启动 Master 发送 */
        g_time_ms = round * 200;
        ping_pong_start(g_slave, PING_PONG_ROLE_SLAVE);

        g_time_ms = round * 200 + 10;
        ping_pong_start(g_master, PING_PONG_ROLE_MASTER);

        /* Master TX done → Slave 收到 Ping */
        g_time_ms += 5;
        if (g_master_tx_done_pending) {
            g_master_tx_done_pending = 0;
            ping_pong_on_tx_done(g_master);
        }

        /* 环回数据 → 交给 Slave */
        if (g_loopback_pending) {
            g_loopback_pending = 0;
            g_time_ms += 5;
            ping_pong_on_rx_done(g_slave, g_loopback_buf, g_loopback_len, -50, 10);
        }

        /* Slave TX done → Master 收到 Pong */
        g_time_ms += 5;
        if (g_slave_tx_done_pending) {
            g_slave_tx_done_pending = 0;
            ping_pong_on_tx_done(g_slave);
        }

        /* 环回数据 → 交给 Master */
        if (g_loopback_pending) {
            g_loopback_pending = 0;
            g_time_ms += 5;
            ping_pong_on_rx_done(g_master, g_loopback_buf, g_loopback_len, -45, 12);
        }

        /* 停止 */
        if (ping_pong_get_state(g_master) != PING_PONG_STATE_IDLE) {
            ping_pong_stop(g_master);
        }
        if (ping_pong_get_state(g_slave) != PING_PONG_STATE_IDLE) {
            ping_pong_stop(g_slave);
        }
    }

    /* 打印统计 */
    ping_pong_stats_t stats;
    ping_pong_get_stats(g_master, &stats);
    printf("\n[Master Stats] success=%u fail=%u last_rtt=%u min_rtt=%u max_rtt=%u\n",
           stats.success_count, stats.fail_count,
           stats.last_rtt_ms, stats.min_rtt_ms, stats.max_rtt_ms);

    printf("\n=== Loopback Example Complete ===\n");
    return 0;
}
