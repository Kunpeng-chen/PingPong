/*
 * PingPong 主机端示例
 *
 * 演示 Master 角色的典型使用流程：
 * - 初始化（重传 3 次，超时时间 3 秒）
 * - 发送 Ping，等待 Pong
 * - 若收到 Pong，则重启下一轮 PingPong
 * - 若超时没收到 Pong，开始重传（重传过程中 seq 不变）
 * - 超过最大重传次数依旧未收到 Pong，则记为失败，开启下一轮 PingPong
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

/* ==================== Master 实例 ==================== */

static uint8_t g_master_mem[512];
#define g_master ((ping_pong_t *)g_master_mem)

/* 重启标志（在回调外延迟执行，避免重入问题） */
static int g_restart_pending;

/* ==================== 回调实现 ==================== */

static void master_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                          void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            /* 构建 Ping 包并模拟发送 */
            build_ping_packet(notify->payload.tx_request.tx_buffer,
                              (uint16_t)notify->seq);
            mock_radio_send(notify->payload.tx_request.tx_buffer,
                            notify->payload.tx_request.tx_buffer_size);
            printf("  [Master] TX_REQUEST: 发送 Ping, seq=%u\n", notify->seq);
            break;

        case PING_PONG_NOTIFY_RX_REQUEST:
            printf("  [Master] RX_REQUEST: 进入接收等待, seq=%u\n", notify->seq);
            break;

        case PING_PONG_NOTIFY_SUCCESS:
            printf("  [Master] SUCCESS: 收到 Pong! RTT=%u ms, RSSI=%d, SNR=%d\n",
                   notify->payload.success.rtt_ms,
                   notify->payload.success.rssi,
                   notify->payload.success.snr);
            /* 成功 → 延迟重启下一轮 */
            g_restart_pending = 1;
            break;

        case PING_PONG_NOTIFY_FAIL:
            printf("  [Master] FAIL: 失败! 原因=%u, seq=%u\n",
                   notify->payload.fail.fail_reason, notify->seq);
            /* 失败 → 延迟重启下一轮 */
            g_restart_pending = 1;
            break;

        case PING_PONG_NOTIFY_RETRY:
            printf("  [Master] RETRY: 第 %u 次重传, seq=%u (seq 不变)\n",
                   notify->payload.retry.retry_count, notify->seq);
            break;

        case PING_PONG_NOTIFY_CONFLICT:
            printf("  [Master] CONFLICT: 冲突类型=%u\n",
                   notify->payload.conflict.conflict_type);
            break;

        default:
            break;
    }
}

/* ==================== 辅助：处理延迟重启 ==================== */

static void handle_pending_restart(void)
{
    if (g_restart_pending) {
        g_restart_pending = 0;
        /* 从 IDLE 状态重新启动 Master，开启下一轮 */
        ping_pong_start(g_master, PING_PONG_ROLE_MASTER);
    }
}

/* ==================== 辅助：完成 TX 并进入 RX_WAIT ==================== */

static void complete_tx(uint32_t time_ms)
{
    if (g_tx_done_pending) {
        g_tx_done_pending = 0;
        g_time_ms = time_ms;
        ping_pong_on_tx_done(g_master);
    }
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== PingPong 主机端示例 ===\n");
    printf("配置: 超时时间=3000ms, 最大重传次数=3\n");

    /* 初始化 */
    memset(g_master_mem, 0, sizeof(g_master_mem));

    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = master_notify,
        .user_data = NULL,
        .trace = NULL,
    };

    ping_pong_config_t config = {
        .timeout_ms = 3000,         /* 超时时间 3 秒 */
        .max_retries = 3,           /* 最大重传 3 次 */
        .tx_buffer_size = 64,       /* 发送缓冲区 */
        .slave_rx_timeout_ms = 0,   /* Master 不使用 */
        .tx_timeout_ms = 0,         /* 不启用 TX 超时保护 */
    };

    assert(ping_pong_init(g_master, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(g_master, &config) == PING_PONG_OK);

    /*
     * 场景 1: 正常收发 (seq=0)
     * 启动 → 发送 Ping → 收到 Pong → SUCCESS → 下一轮
     */
    {
        uint8_t pong[6];
        printf("\n--- 场景 1: 正常收发 (seq=0) ---\n");

        g_time_ms = 0;
        ping_pong_start(g_master, PING_PONG_ROLE_MASTER);

        complete_tx(10);

        build_pong_packet(pong, 0);
        g_time_ms = 50;
        ping_pong_on_rx_done(g_master, pong, 6, -50, 10);

        /* SUCCESS → restart_pending=1 → 开启下一轮 */
        handle_pending_restart();
    }

    /*
     * 场景 2: 超时重传后成功 (seq=1)
     * 发送 Ping → 超时 → 重传 Ping (seq=1 不变) → 收到 Pong → SUCCESS → 下一轮
     */
    {
        uint8_t pong[6];
        printf("\n--- 场景 2: 超时重传后成功 (seq=1) ---\n");

        /* 上一轮 restart 已触发 start，TX_REQUEST 已发出 */
        complete_tx(10010);

        /* 等待超过 3000ms → 超时触发重传 */
        g_time_ms = 13100;
        ping_pong_process(g_master);

        /* 重传 TX 完成 */
        complete_tx(13110);

        /* 这次收到了 Pong */
        build_pong_packet(pong, 1);
        g_time_ms = 13150;
        ping_pong_on_rx_done(g_master, pong, 6, -55, 8);

        handle_pending_restart();
    }

    /*
     * 场景 3: 超过最大重传次数 (seq=2)
     * Ping → 超时 → 重传1 → 超时 → 重传2 → 超时 → 重传3 → 超时 → FAIL → 下一轮
     */
    {
        uint32_t retry;
        uint32_t base_time = 20000;
        printf("\n--- 场景 3: 超过最大重传次数 (seq=2) ---\n");

        /* 上一轮 restart 已触发 start */
        complete_tx(base_time + 10);

        /* 3 次重传，每次都超时 */
        for (retry = 0; retry < 3; retry++) {
            uint32_t retry_time = base_time + (retry + 1) * 3100;

            /* 超时触发重传 */
            g_time_ms = retry_time;
            ping_pong_process(g_master);

            /* 重传 TX 完成 */
            complete_tx(retry_time + 10);
        }

        /* 最后一次超时 → 超过 max_retries → FAIL */
        g_time_ms = base_time + 4 * 3100;
        ping_pong_process(g_master);

        handle_pending_restart();
    }

    /* 打印最终统计 */
    ping_pong_stats_t stats;
    ping_pong_get_stats(g_master, &stats);
    printf("\n========== 最终统计 ==========\n");
    printf("成功次数: %u\n", stats.success_count);
    printf("失败次数: %u\n", stats.fail_count);
    printf("重传次数: %u\n", stats.retry_count);
    printf("发送 Ping 次数: %u\n", stats.master_tx_count);
    printf("最近 RTT: %u ms\n", stats.last_rtt_ms);
    printf("最小 RTT: %u ms\n", stats.min_rtt_ms);
    printf("最大 RTT: %u ms\n", stats.max_rtt_ms);
    printf("连续成功: %u\n", stats.consecutive_success_count);
    printf("连续失败: %u\n", stats.consecutive_fail_count);

    printf("\n=== 主机端示例完成 ===\n");
    return 0;
}
