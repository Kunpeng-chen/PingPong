/*
 * PingPong 多实例示例
 *
 * 演示在同一进程中创建两个独立的 PingPong 实例（均为 Master），
 * 利用 user_data 区分回调来源。
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

static void build_pong(uint8_t *buf, uint16_t seq)
{
    buf[0] = 0x02;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    uint16_t crc = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== 实例定义 ==================== */

typedef struct {
    int id;
    const char *name;
} instance_ctx_t;

static void multi_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                         void *user_data)
{
    instance_ctx_t *ctx = (instance_ctx_t *)user_data;
    (void)pp;
    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST: {
            /* Build Ping packet in tx_buffer */
            uint8_t *buf = notify->payload.tx_request.tx_buffer;
            buf[0] = 0x01;
            buf[1] = (uint8_t)(notify->seq >> 8);
            buf[2] = (uint8_t)(notify->seq & 0xFF);
            buf[3] = 0;
            uint16_t crc = crc16_ccitt(buf, 4);
            buf[4] = (uint8_t)(crc >> 8);
            buf[5] = (uint8_t)(crc & 0xFF);
            printf("  [Instance %d: %s] TX_REQUEST seq=%u\n", ctx->id, ctx->name, notify->seq);
            break;
        }
        case PING_PONG_NOTIFY_SUCCESS:
            printf("  [Instance %d: %s] SUCCESS rtt=%u ms\n",
                   ctx->id, ctx->name, notify->payload.success.rtt_ms);
            break;
        case PING_PONG_NOTIFY_FAIL:
            printf("  [Instance %d: %s] FAIL reason=%u\n",
                   ctx->id, ctx->name, notify->payload.fail.fail_reason);
            break;
        case PING_PONG_NOTIFY_RX_PONG:
            printf("  [Instance %d: %s] RX_PONG RSSI=%d SNR=%d\n",
                   ctx->id, ctx->name,
                   notify->payload.rx_pong.rssi, notify->payload.rx_pong.snr);
            break;
        default:
            break;
    }
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== PingPong Multi-Instance Example ===\n\n");

    static uint8_t mem1[512];
    static uint8_t mem2[512];
    #define pp1 ((ping_pong_t *)mem1)
    #define pp2 ((ping_pong_t *)mem2)

    static instance_ctx_t ctx1 = { .id = 1, .name = "Channel-A" };
    static instance_ctx_t ctx2 = { .id = 2, .name = "Channel-B" };

    ping_pong_port_t port1 = {
        .get_time_ms = mock_get_time_ms,
        .notify = multi_notify,
        .user_data = &ctx1,
        .trace = NULL,
    };
    ping_pong_port_t port2 = {
        .get_time_ms = mock_get_time_ms,
        .notify = multi_notify,
        .user_data = &ctx2,
        .trace = NULL,
    };

    ping_pong_config_t config = {
        .timeout_ms = 100,
        .max_retries = 3,
        .tx_buffer_size = 64,
        .slave_rx_timeout_ms = 0,
        .tx_timeout_ms = 0,
    };

    memset(mem1, 0, sizeof(mem1));
    memset(mem2, 0, sizeof(mem2));
    assert(ping_pong_init(pp1, &port1) == PING_PONG_OK);
    assert(ping_pong_init(pp2, &port2) == PING_PONG_OK);
    assert(ping_pong_set_config(pp1, &config) == PING_PONG_OK);
    assert(ping_pong_set_config(pp2, &config) == PING_PONG_OK);

    /* Both instances run as Master with independent sessions */
    printf("--- Starting both instances as Master ---\n");
    g_time_ms = 100;
    ping_pong_start(pp1, PING_PONG_ROLE_MASTER);
    ping_pong_start(pp2, PING_PONG_ROLE_MASTER);

    /* Instance 1: TX done + receive Pong */
    g_time_ms = 110;
    ping_pong_on_tx_done(pp1);

    uint8_t pong[6];
    build_pong(pong, 0);
    g_time_ms = 130;
    ping_pong_on_rx_done(pp1, pong, 6, -50, 10);

    /* Instance 2: TX done + receive Pong (with different timing) */
    g_time_ms = 115;
    ping_pong_on_tx_done(pp2);

    build_pong(pong, 0);
    g_time_ms = 155;
    ping_pong_on_rx_done(pp2, pong, 6, -55, 8);

    /* Print stats */
    ping_pong_stats_t stats1, stats2;
    ping_pong_get_stats(pp1, &stats1);
    ping_pong_get_stats(pp2, &stats2);

    printf("\n[Instance 1] success=%u rtt=%u\n", stats1.success_count, stats1.last_rtt_ms);
    printf("[Instance 2] success=%u rtt=%u\n", stats2.success_count, stats2.last_rtt_ms);

    printf("\n=== Multi-Instance Example Complete ===\n");
    return 0;
}
