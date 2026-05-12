/*
 * Phase 2 regression tests: Master should tolerate corrupted or unrelated
 * packets until RX timeout/retry, instead of failing immediately.
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

static const ping_pong_notify_t *find_last_notify(ping_pong_notify_type_t type)
{
    int i;
    for (i = g_notify_count - 1; i >= 0; i--) {
        if (g_notifications[i].type == type) {
            return &g_notifications[i];
        }
    }
    return NULL;
}

static void reset_recording(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    memset(g_notifications, 0, sizeof(g_notifications));
}

static void init_master(ping_pong_t *pp)
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
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
}

static void test_bad_pong_crc_is_ignored_until_timeout(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_master(pp);
    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[4] ^= 0x55u;

    g_time_ms = 10;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.crc_error_count == 1);
    assert(stats.fail_count == 0);

    g_time_ms = 100;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 2);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count == 1);

    free(mem);
    printf("  PASS: test_bad_pong_crc_is_ignored_until_timeout\n");
}

static void test_bad_packet_then_good_pong_succeeds(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    const ping_pong_notify_t *notify;

    init_master(pp);

    assert(ping_pong_build_pong(packet, sizeof(packet), 99) == PING_PONG_OK);
    g_time_ms = 10;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -50, 8) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);

    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    g_time_ms = 20;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -50, 8) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);

    packet[0] = 0xAAu;
    packet[1] = 0u;
    packet[2] = 0u;
    packet[3] = 0u;
    packet[4] = 0u;
    packet[5] = 0u;
    g_time_ms = 30;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -50, 8) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    g_time_ms = 40;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -45, 9) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    notify = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(notify != NULL);
    assert(notify->payload.success.rtt_ms == 40);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 2);
    assert(stats.conflict_count == 1);
    assert(stats.success_count == 1);
    assert(stats.fail_count == 0);

    free(mem);
    printf("  PASS: test_bad_packet_then_good_pong_succeeds\n");
}

int main(void)
{
    printf("Running PingPong Phase 2 Master bad-packet tolerance tests...\n");
    test_bad_pong_crc_is_ignored_until_timeout();
    test_bad_packet_then_good_pong_succeeds();
    printf("All PingPong Phase 2 tests passed.\n");
    return 0;
}
