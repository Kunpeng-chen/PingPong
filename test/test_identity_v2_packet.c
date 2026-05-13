#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTIFICATIONS 32

static uint32_t g_time_ms;
static ping_pong_notify_t g_notifications[MAX_NOTIFICATIONS];
static int g_notify_count;

static uint32_t mock_get_time_ms(void)
{
    return g_time_ms;
}

static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                        void *user_data)
{
    (void)pp;
    (void)user_data;
    if (g_notify_count < MAX_NOTIFICATIONS) {
        g_notifications[g_notify_count++] = *notify;
    }
}

static uint8_t *alloc_instance(void)
{
    uint8_t *mem = (uint8_t *)calloc(1u, ping_pong_instance_size() + 64u);
    assert(mem != NULL);
    return mem;
}

static void reset_recording(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    memset(g_notifications, 0, sizeof(g_notifications));
}

static int count_notify(ping_pong_notify_type_t type)
{
    int count = 0;
    int i;
    for (i = 0; i < g_notify_count; i++) {
        if (g_notifications[i].type == type) {
            count++;
        }
    }
    return count;
}

static void init_with_identity(ping_pong_t *pp, uint32_t network_id,
                               uint16_t src_id, uint16_t dst_id)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };
    ping_pong_config_t config;

    reset_recording();
    ping_pong_get_default_config(&config);
    config.max_retries = 1;
    config.rx_timeout_ms = 100;
    config.tx_timeout_ms = 0;
    config.network_id = network_id;
    config.src_id = src_id;
    config.dst_id = dst_id;

    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
}

static void test_v2_packet_layout_and_size(void)
{
    uint8_t packet[PING_PONG_PACKET_SIZE];

    assert(PING_PONG_PACKET_SIZE == PING_PONG_V2_PACKET_SIZE);
    assert(PING_PONG_MIN_PACKET_SIZE == PING_PONG_V1_PACKET_SIZE);
    assert(ping_pong_build_ping_ex(packet, sizeof(packet), 0x1234u,
                                   0xAABBCCDDu, 0x0102u, 0x0304u) == PING_PONG_OK);
    assert(packet[0] == 0x01u);
    assert(packet[1] == 0x12u);
    assert(packet[2] == 0x34u);
    assert(packet[3] == PING_PONG_PROTOCOL_VERSION_V2);
    assert(packet[4] == 0xAAu && packet[5] == 0xBBu);
    assert(packet[6] == 0xCCu && packet[7] == 0xDDu);
    assert(packet[8] == 0x01u && packet[9] == 0x02u);
    assert(packet[10] == 0x03u && packet[11] == 0x04u);

    printf("  PASS: test_v2_packet_layout_and_size\n");
}

static void test_master_accepts_matching_identity_pong(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t pong[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_with_identity(pp, 0x01020304u, 0x1001u, 0x2002u);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(g_notifications[0].payload.tx_request.tx_len == PING_PONG_PACKET_SIZE);
    assert(g_notifications[0].payload.tx_request.tx_buffer[3] == PING_PONG_PROTOCOL_VERSION_V2);
    assert(g_notifications[0].payload.tx_request.tx_buffer[8] == 0x10u);
    assert(g_notifications[0].payload.tx_request.tx_buffer[9] == 0x01u);
    assert(g_notifications[0].payload.tx_request.tx_buffer[10] == 0x20u);
    assert(g_notifications[0].payload.tx_request.tx_buffer[11] == 0x02u);

    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_pong_ex(pong, sizeof(pong), 0,
                                   0x01020304u, 0x2002u, 0x1001u) == PING_PONG_OK);
    g_time_ms = 42;
    assert(ping_pong_on_rx_done(pp, pong, sizeof(pong), -40, 8) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.success_count == 1);
    assert(stats.rx_count == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);

    free(mem);
    printf("  PASS: test_master_accepts_matching_identity_pong\n");
}

static void test_master_rejects_wrong_network_until_timeout(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t pong[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_with_identity(pp, 0x01020304u, 0x1001u, 0x2002u);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_pong_ex(pong, sizeof(pong), 0,
                                   0x11111111u, 0x2002u, 0x1001u) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, pong, sizeof(pong), -40, 8) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 1);
    assert(stats.success_count == 0);

    free(mem);
    printf("  PASS: test_master_rejects_wrong_network_until_timeout\n");
}

static void test_slave_replies_with_reversed_identity(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t ping[PING_PONG_PACKET_SIZE];
    const ping_pong_notify_t *tx;

    init_with_identity(pp, 0x01020304u, 0x2002u, 0x1001u);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_build_ping_ex(ping, sizeof(ping), 7,
                                   0x01020304u, 0x1001u, 0x2002u) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, ping, sizeof(ping), -35, 10) == PING_PONG_OK);
    tx = &g_notifications[g_notify_count - 1];
    assert(tx->type == PING_PONG_NOTIFY_TX_REQUEST);
    assert(tx->seq == 7);
    assert(tx->payload.tx_request.tx_buffer[0] == 0x02u);
    assert(tx->payload.tx_request.tx_buffer[3] == PING_PONG_PROTOCOL_VERSION_V2);
    assert(tx->payload.tx_request.tx_buffer[8] == 0x20u);
    assert(tx->payload.tx_request.tx_buffer[9] == 0x02u);
    assert(tx->payload.tx_request.tx_buffer[10] == 0x10u);
    assert(tx->payload.tx_request.tx_buffer[11] == 0x01u);

    free(mem);
    printf("  PASS: test_slave_replies_with_reversed_identity\n");
}

int main(void)
{
    printf("Running PingPong v2 identity packet tests...\n");
    test_v2_packet_layout_and_size();
    test_master_accepts_matching_identity_pong();
    test_master_rejects_wrong_network_until_timeout();
    test_slave_replies_with_reversed_identity();
    printf("All PingPong v2 identity packet tests passed.\n");
    return 0;
}
