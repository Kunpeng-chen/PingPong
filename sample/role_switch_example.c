/*
 * PingPong 运行时角色切换示例
 *
 * 演示同一实例在不同轮次中切换 Master/Slave 角色。
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

/* ==================== 通知记录 ==================== */

static void on_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                      void *user_data)
{
    const char *label = (const char *)user_data;
    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            if (ping_pong_get_role(pp) == PING_PONG_ROLE_MASTER) {
                build_packet(notify->payload.tx_request.tx_buffer, 0x01, (uint16_t)notify->seq);
            } else {
                build_packet(notify->payload.tx_request.tx_buffer, 0x02, (uint16_t)notify->seq);
            }
            printf("  [%s] TX_REQUEST seq=%u\n", label, notify->seq);
            break;
        case PING_PONG_NOTIFY_SUCCESS:
            printf("  [%s] SUCCESS rtt=%u\n", label, notify->payload.success.rtt_ms);
            break;
        case PING_PONG_NOTIFY_PING_RECEIVED:
            printf("  [%s] PING_RECEIVED seq=%u\n", label, notify->seq);
            break;
        default:
            break;
    }
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== PingPong Role Switch Example ===\n\n");

    static uint8_t mem_a[512];
    static uint8_t mem_b[512];
    #define pp_a ((ping_pong_t *)mem_a)
    #define pp_b ((ping_pong_t *)mem_b)

    static const char *label_a = "DevA";
    static const char *label_b = "DevB";

    ping_pong_port_t port_a = {
        .get_time_ms = mock_get_time_ms,
        .notify = on_notify,
        .user_data = (void *)label_a,
        .trace = NULL,
    };
    ping_pong_port_t port_b = {
        .get_time_ms = mock_get_time_ms,
        .notify = on_notify,
        .user_data = (void *)label_b,
        .trace = NULL,
    };

    ping_pong_config_t config = {
        .timeout_ms = 100,
        .max_retries = 3,
        .tx_buffer_size = 64,
        .slave_rx_timeout_ms = 5000,
        .tx_timeout_ms = 0,
    };

    memset(mem_a, 0, sizeof(mem_a));
    memset(mem_b, 0, sizeof(mem_b));
    assert(ping_pong_init(pp_a, &port_a) == PING_PONG_OK);
    assert(ping_pong_init(pp_b, &port_b) == PING_PONG_OK);
    assert(ping_pong_set_config(pp_a, &config) == PING_PONG_OK);
    assert(ping_pong_set_config(pp_b, &config) == PING_PONG_OK);

    /* Round 1: A=Master, B=Slave */
    printf("--- Round 1: A=Master, B=Slave ---\n");
    g_time_ms = 100;
    ping_pong_start(pp_b, PING_PONG_ROLE_SLAVE);
    ping_pong_start(pp_a, PING_PONG_ROLE_MASTER);

    g_time_ms = 105;
    ping_pong_on_tx_done(pp_a);

    /* Simulate: A's packet → B */
    uint8_t pkt[6];
    build_packet(pkt, 0x01, 0);
    g_time_ms = 110;
    ping_pong_on_rx_done(pp_b, pkt, 6, -50, 10);

    g_time_ms = 115;
    ping_pong_on_tx_done(pp_b);

    /* B's response → A */
    build_packet(pkt, 0x02, 0);
    g_time_ms = 120;
    ping_pong_on_rx_done(pp_a, pkt, 6, -45, 12);

    ping_pong_stop(pp_a);
    ping_pong_stop(pp_b);

    /* Round 2: A=Slave, B=Master (roles switched!) */
    printf("\n--- Round 2: A=Slave, B=Master (switched!) ---\n");
    g_time_ms = 300;
    ping_pong_start(pp_a, PING_PONG_ROLE_SLAVE);
    ping_pong_start(pp_b, PING_PONG_ROLE_MASTER);

    g_time_ms = 305;
    ping_pong_on_tx_done(pp_b);

    build_packet(pkt, 0x01, 0);
    g_time_ms = 310;
    ping_pong_on_rx_done(pp_a, pkt, 6, -48, 11);

    g_time_ms = 315;
    ping_pong_on_tx_done(pp_a);

    build_packet(pkt, 0x02, 0);
    g_time_ms = 320;
    ping_pong_on_rx_done(pp_b, pkt, 6, -47, 11);

    ping_pong_stop(pp_a);
    ping_pong_stop(pp_b);

    printf("\n=== Role Switch Example Complete ===\n");
    return 0;
}
