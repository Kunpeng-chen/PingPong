/*
 * Phase 3 tests: v2 identity fields and address/network filtering.
 */

#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTIFICATIONS 64
#define TEST_CTX_EXTRA_BYTES 256u

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
    uint8_t *mem = (uint8_t *)calloc(1u, ping_pong_instance_size() + TEST_CTX_EXTRA_BYTES);
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

static void init_with_identity(ping_pong_t *pp, const ping_pong_identity_t *identity)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };
    ping_pong_config_t config = {
        .max_retries = 1,
        .rx_timeout_ms = 100,
        .tx_timeout_ms = 0,
    };

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
    assert(ping_pong_set_identity(pp, identity) == PING_PONG_OK);
}

static void patch_crc(uint8_t *packet)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    for (i = 0; i < 22u; i++) {
        uint8_t j;
        crc ^= (uint16_t)packet[i] << 8;
        for (j = 0; j < 8u; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    packet[22] = (uint8_t)(crc >> 8);
    packet[23] = (uint8_t)(crc & 0xFFu);
}

static void test_v2_packet_layout_and_default_identity(void)
{
    uint8_t ping[PING_PONG_PACKET_SIZE];
    uint8_t pong[PING_PONG_PACKET_SIZE];

    assert(PING_PONG_PACKET_SIZE == 24u);
    assert(ping_pong_build_ping(ping, sizeof(ping), 0x1234u) == PING_PONG_OK);
    assert(ping[0] == 0x50u);
    assert(ping[1] == 0x50u);
    assert(ping[2] == PING_PONG_PROTOCOL_VERSION);
    assert(ping[3] == 0x01u);
    assert(ping[5] == 0x12u);
    assert(ping[6] == 0x34u);
    assert(ping[7] == PING_PONG_DEFAULT_NETWORK_ID);
    assert(ping[8] == PING_PONG_DEFAULT_LOCAL_ID);
    assert(ping[9] == PING_PONG_DEFAULT_PEER_ID);

    assert(ping_pong_build_pong(pong, sizeof(pong), 0x1234u) == PING_PONG_OK);
    assert(pong[3] == 0x02u);
    assert(pong[8] == PING_PONG_DEFAULT_PEER_ID);
    assert(pong[9] == PING_PONG_DEFAULT_LOCAL_ID);

    printf("  PASS: test_v2_packet_layout_and_default_identity\n");
}

static void test_master_filters_network_and_address(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    ping_pong_identity_t master = {
        .local_id = 10u,
        .peer_id = 20u,
        .network_id = 7u,
        .allow_broadcast = 0u,
    };

    init_with_identity(pp, &master);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 8u;
    patch_crc(packet);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 7u;
    packet[8] = 20u;
    packet[9] = 11u;
    patch_crc(packet);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 7u;
    packet[8] = 20u;
    packet[9] = 10u;
    patch_crc(packet);
    g_time_ms = 20;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(count_notify(PING_PONG_NOTIFY_SUCCESS) == 1);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.network_filter_count == 1u);
    assert(stats.address_filter_count == 1u);
    assert(stats.success_count == 1u);
    assert(stats.fail_count == 0u);

    free(mem);
    printf("  PASS: test_master_filters_network_and_address\n");
}

static void test_slave_filters_network_address_and_broadcast_default(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    ping_pong_identity_t slave = {
        .local_id = 20u,
        .peer_id = 10u,
        .network_id = 7u,
        .allow_broadcast = 0u,
    };

    init_with_identity(pp, &slave);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 8u;
    packet[8] = 10u;
    packet[9] = 20u;
    patch_crc(packet);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 7u;
    packet[8] = 10u;
    packet[9] = PING_PONG_BROADCAST_ID;
    patch_crc(packet);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[7] = 7u;
    packet[8] = 10u;
    packet[9] = 20u;
    patch_crc(packet);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.network_filter_count == 1u);
    assert(stats.address_filter_count == 1u);
    assert(stats.rx_count == 1u);

    free(mem);
    printf("  PASS: test_slave_filters_network_address_and_broadcast_default\n");
}

static void test_identity_setter_guards(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_identity_t id = {
        .local_id = 1u,
        .peer_id = 2u,
        .network_id = 3u,
        .allow_broadcast = 0u,
    };
    ping_pong_identity_t invalid = id;

    init_with_identity(pp, &id);
    assert(ping_pong_set_identity(pp, NULL) == PING_PONG_ERR_NULL_PTR);
    invalid.local_id = PING_PONG_BROADCAST_ID;
    assert(ping_pong_set_identity(pp, &invalid) == PING_PONG_ERR_INVALID_PARAM);

    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_set_identity(pp, &id) == PING_PONG_ERR_INVALID_STATE);

    free(mem);
    printf("  PASS: test_identity_setter_guards\n");
}

int main(void)
{
    printf("Running PingPong Phase 3 identity/v2 tests...\n");
    test_v2_packet_layout_and_default_identity();
    test_master_filters_network_and_address();
    test_slave_filters_network_address_and_broadcast_default();
    test_identity_setter_guards();
    printf("All PingPong Phase 3 tests passed.\n");
    return 0;
}
