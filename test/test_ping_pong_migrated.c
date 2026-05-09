/*
 * PingPong legacy regression tests migrated to the current notification model.
 */

#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTIFICATIONS 64
#define TEST_EXTRA_BYTES  256u

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
    uint8_t *mem = (uint8_t *)calloc(1u, ping_pong_instance_size() + TEST_EXTRA_BYTES);
    assert(mem != NULL);
    return mem;
}

static void reset_recording(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    memset(g_notifications, 0, sizeof(g_notifications));
}

static const ping_pong_notify_t *last_notify(ping_pong_notify_type_t type)
{
    int i;
    for (i = g_notify_count - 1; i >= 0; i--) {
        if (g_notifications[i].type == type) {
            return &g_notifications[i];
        }
    }
    return NULL;
}

static void init_instance(ping_pong_t *pp, const ping_pong_config_t *config)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    if (config != NULL) {
        assert(ping_pong_set_config(pp, config) == PING_PONG_OK);
    }
}

static ping_pong_config_t default_config(void)
{
    ping_pong_config_t config = {
        .max_retries = 3,
        .rx_timeout_ms = 100,
        .tx_timeout_ms = 0,
    };
    return config;
}

static void test_init_config_and_lifecycle(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    ping_pong_stats_t stats;

    assert(ping_pong_init(NULL, &(ping_pong_port_t){mock_get_time_ms, mock_notify, NULL, NULL}) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_set_config(NULL, &config) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_get_stats(NULL, &stats) == PING_PONG_ERR_NULL_PTR);

    init_instance(pp, &config);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_NONE);
    assert(ping_pong_is_valid(pp) == 1);
    assert(ping_pong_stop(pp) == PING_PONG_ERR_INVALID_STATE);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_stop(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_STOPPED);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_NONE);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.success_count == 0);
    assert(stats.fail_count == 0);

    free(mem);
    printf("  PASS: test_init_config_and_lifecycle\n");
}

static void test_master_success_and_restart_seq(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    uint8_t pong[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    const ping_pong_notify_t *notify;

    init_instance(pp, &config);
    g_time_ms = 1000;
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->payload.tx_request.tx_len == PING_PONG_PACKET_SIZE);

    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);

    assert(ping_pong_build_pong(pong, sizeof(pong), 0) == PING_PONG_OK);
    g_time_ms = 1055;
    assert(ping_pong_on_rx_done(pp, pong, sizeof(pong), -50, 10) == PING_PONG_OK);

    notify = last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(notify != NULL);
    assert(notify->payload.success.rtt_ms == 55);
    assert(notify->payload.success.rssi == -50);
    assert(notify->payload.success.snr == 10);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.success_count == 1);
    assert(stats.rx_count == 1);
    assert(stats.last_rtt_ms == 55);

    reset_recording();
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->seq == 1);

    free(mem);
    printf("  PASS: test_master_success_and_restart_seq\n");
}

static void test_master_retry_fail_and_error_paths(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    const ping_pong_notify_t *fail;

    config.max_retries = 1;
    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = 101;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = 202;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    fail = last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fail != NULL);
    assert(fail->payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count == 1);
    assert(stats.rx_timeout_count == 1);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[4] ^= 0x55u;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    fail = last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fail != NULL);
    assert(fail->payload.fail.fail_reason == PING_PONG_FAIL_REASON_CRC_ERROR);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_pong(packet, sizeof(packet), 99) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    fail = last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fail != NULL);
    assert(fail->payload.fail.fail_reason == PING_PONG_FAIL_REASON_PARSE_ERROR);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    fail = last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fail != NULL);
    assert(fail->payload.fail.fail_reason == PING_PONG_FAIL_REASON_CONFLICT);

    free(mem);
    printf("  PASS: test_master_retry_fail_and_error_paths\n");
}

static void test_slave_flow_timeout_and_no_timeout(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);

    assert(ping_pong_build_ping(packet, sizeof(packet), 42) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 8) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->payload.tx_request.tx_len == PING_PONG_PACKET_SIZE);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_count == 1);
    assert(stats.last_rssi == -30);
    assert(stats.last_snr == 8);

    reset_recording();
    g_time_ms = 1000;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_timeout_count == 1);
    assert(last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    config.rx_timeout_ms = 0;
    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    reset_recording();
    g_time_ms = 999999;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(g_notify_count == 0);

    free(mem);
    printf("  PASS: test_slave_flow_timeout_and_no_timeout\n");
}

static void test_slave_error_paths(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);

    assert(ping_pong_build_ping(packet, sizeof(packet), 1) == PING_PONG_OK);
    packet[4] ^= 0x11u;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 8) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.crc_error_count == 1);

    assert(ping_pong_build_pong(packet, sizeof(packet), 1) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 8) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.conflict_count == 1);

    packet[0] = 0xAAu;
    packet[1] = packet[2] = packet[3] = packet[4] = packet[5] = 0;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 8) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 1);

    free(mem);
    printf("  PASS: test_slave_error_paths\n");
}

static void test_tx_timeout_and_invalid_calls(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = default_config();
    ping_pong_stats_t stats;
    uint8_t packet[PING_PONG_PACKET_SIZE];

    config.max_retries = 1;
    config.tx_timeout_ms = 10;
    init_instance(pp, &config);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_rx_done(pp, NULL, sizeof(packet), 0, 0) == PING_PONG_ERR_NULL_PTR);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    g_time_ms = 11;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.tx_timeout_count == 1);
    assert(stats.retry_count == 1);
    g_time_ms = 22;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_FAIL) != NULL);
    assert(last_notify(PING_PONG_NOTIFY_FAIL)->payload.fail.fail_reason == PING_PONG_FAIL_REASON_TX_TIMEOUT);

    free(mem);
    printf("  PASS: test_tx_timeout_and_invalid_calls\n");
}

static void test_packet_build_helpers(void)
{
    uint8_t ping[PING_PONG_PACKET_SIZE];
    uint8_t pong[PING_PONG_PACKET_SIZE];
    uint8_t small[PING_PONG_PACKET_SIZE - 1u];

    assert(ping_pong_build_ping(NULL, sizeof(ping), 0) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_build_ping(small, sizeof(small), 0) == PING_PONG_ERR_INVALID_PARAM);
    assert(ping_pong_build_ping(ping, sizeof(ping), 0x1234u) == PING_PONG_OK);
    assert(ping_pong_build_pong(pong, sizeof(pong), 0x1234u) == PING_PONG_OK);
    assert(ping[0] == 0x01u);
    assert(pong[0] == 0x02u);
    assert(ping[1] == 0x12u);
    assert(ping[2] == 0x34u);
    assert(memcmp(ping, pong, sizeof(ping)) != 0);

    printf("  PASS: test_packet_build_helpers\n");
}

int main(void)
{
    printf("Running migrated PingPong legacy tests...\n");
    test_init_config_and_lifecycle();
    test_master_success_and_restart_seq();
    test_master_retry_fail_and_error_paths();
    test_slave_flow_timeout_and_no_timeout();
    test_slave_error_paths();
    test_tx_timeout_and_invalid_calls();
    test_packet_build_helpers();
    printf("All migrated PingPong legacy tests passed.\n");
    return 0;
}
