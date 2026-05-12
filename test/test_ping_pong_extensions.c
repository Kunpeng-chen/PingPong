/*
 * PingPong focused regression tests for the optimized API.
 */

#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CTX_EXTRA_BYTES 256u
#define MAX_NOTIFICATIONS   128

static uint32_t g_time_ms;
static ping_pong_notify_t g_notifications[MAX_NOTIFICATIONS];
static int g_notify_count;
static int g_trace_count;
static int g_user_data_value = 42;
static void *g_seen_user_data;

static uint32_t mock_get_time_ms(void)
{
    return g_time_ms;
}

static void mock_trace(const char *msg)
{
    assert(msg != NULL);
    g_trace_count++;
}

static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                        void *user_data)
{
    (void)pp;
    g_seen_user_data = user_data;
    if (g_notify_count < MAX_NOTIFICATIONS) {
        g_notifications[g_notify_count++] = *notify;
    }
}

static uint8_t *alloc_instance(void)
{
    uint32_t size = ping_pong_instance_size() + TEST_CTX_EXTRA_BYTES;
    uint8_t *mem = (uint8_t *)calloc(1u, size);
    assert(mem != NULL);
    return mem;
}

static void reset_recording(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    g_trace_count = 0;
    g_seen_user_data = NULL;
    memset(g_notifications, 0, sizeof(g_notifications));
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

static int count_notify(ping_pong_notify_type_t type)
{
    int i;
    int count = 0;
    for (i = 0; i < g_notify_count; i++) {
        if (g_notifications[i].type == type) {
            count++;
        }
    }
    return count;
}

static void init_with_config(ping_pong_t *pp, const ping_pong_config_t *config)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = &g_user_data_value,
        .trace = mock_trace,
    };

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(g_trace_count > 0);
    assert(ping_pong_set_config(pp, config) == PING_PONG_OK);
}

static void init_master(ping_pong_t *pp)
{
    ping_pong_config_t config = {
        .max_retries = 1,
        .rx_timeout_ms = 100,
        .tx_timeout_ms = 10,
    };
    init_with_config(pp, &config);
}

static void init_slave(ping_pong_t *pp, uint32_t rx_timeout_ms)
{
    ping_pong_config_t config = {
        .max_retries = 0,
        .rx_timeout_ms = rx_timeout_ms,
        .tx_timeout_ms = 10,
    };
    init_with_config(pp, &config);
}

static void start_master_rx_wait(ping_pong_t *pp)
{
    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
}

static void test_build_packet_helpers(void)
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
    assert(ping[1] == 0x12u && ping[2] == 0x34u);
    assert(memcmp(ping, pong, sizeof(ping)) != 0);

    printf("  PASS: test_build_packet_helpers\n");
}

static void test_init_and_config_errors(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = {
        .max_retries = 1,
        .rx_timeout_ms = 100,
        .tx_timeout_ms = 0,
    };
    ping_pong_port_t good_port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };
    ping_pong_port_t bad_port = good_port;

    reset_recording();
    assert(ping_pong_init(NULL, &good_port) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_init(pp, NULL) == PING_PONG_ERR_NULL_PTR);
    bad_port.get_time_ms = NULL;
    assert(ping_pong_init(pp, &bad_port) == PING_PONG_ERR_NULL_PTR);
    bad_port = good_port;
    bad_port.notify = NULL;
    assert(ping_pong_init(pp, &bad_port) == PING_PONG_ERR_NULL_PTR);

    memset(mem, 0, ping_pong_instance_size());
    assert(ping_pong_set_config(NULL, &config) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_set_config(pp, NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_get_stats(pp, &(ping_pong_stats_t){0}) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_is_valid(pp) == 0);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_NONE);

    assert(ping_pong_init(pp, &good_port) == PING_PONG_OK);
    assert(ping_pong_is_valid(pp) == 1);
    assert(ping_pong_start(pp, PING_PONG_ROLE_NONE) == PING_PONG_ERR_INVALID_PARAM);

    config.rx_timeout_ms = 0;
    config.max_retries = 1;
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_PARAM);

    config.rx_timeout_ms = 100;
    config.max_retries = 0;
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_PARAM);

    free(mem);
    printf("  PASS: test_init_and_config_errors\n");
}

static void test_state_lifecycle_errors(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_stats_t stats;
    uint8_t packet[PING_PONG_PACKET_SIZE];

    assert(ping_pong_process(NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_stop(NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_reset(NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_on_tx_done(NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_on_rx_done(NULL, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_get_stats(NULL, &stats) == PING_PONG_ERR_NULL_PTR);

    memset(mem, 0, ping_pong_instance_size());
    assert(ping_pong_process(pp) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_stop(pp) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_reset(pp) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_ERR_NOT_INITIALIZED);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_NOT_INITIALIZED);

    init_master(pp);
    assert(ping_pong_stop(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_get_stats(pp, NULL) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_process(pp) == PING_PONG_OK);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(g_seen_user_data == &g_user_data_value);
    assert(ping_pong_set_config(pp, &(ping_pong_config_t){1, 100, 0}) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_stop(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_STOPPED);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_stop(pp) == PING_PONG_OK);
    assert(ping_pong_reset(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_NONE);

    free(mem);
    printf("  PASS: test_state_lifecycle_errors\n");
}

static void test_tx_len_and_core_built_packet(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t expected[PING_PONG_PACKET_SIZE];

    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);

    assert(g_notify_count == 1);
    assert(g_notifications[0].type == PING_PONG_NOTIFY_TX_REQUEST);
    assert(g_notifications[0].payload.tx_request.tx_len == PING_PONG_PACKET_SIZE);
    assert(g_notifications[0].payload.tx_request.tx_buffer_size == PING_PONG_TX_BUFFER_SIZE);
    assert(g_notifications[0].payload.tx_request.tx_buffer != NULL);

    assert(ping_pong_build_ping(expected, sizeof(expected), 0) == PING_PONG_OK);
    assert(memcmp(g_notifications[0].payload.tx_request.tx_buffer,
                  expected,
                  PING_PONG_PACKET_SIZE) == 0);

    free(mem);
    printf("  PASS: test_tx_len_and_core_built_packet\n");
}

static void test_master_success_retry_and_fail(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;
    const ping_pong_notify_t *notify;

    init_master(pp);
    g_time_ms = 100;
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    g_time_ms = 120;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    g_time_ms = 150;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -55, 9) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    notify = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(notify != NULL);
    assert(notify->payload.success.rtt_ms == 50);
    assert(notify->payload.success.rssi == -55);
    assert(notify->payload.success.snr == 9);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.success_count == 1);
    assert(stats.rx_count == 1);
    assert(stats.last_rtt_ms == 50);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(g_notifications[g_notify_count - 1].seq == 1);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms += 150;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) >= 3);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms += 150;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    notify = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(notify != NULL);
    assert(notify->payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count >= 1);
    assert(stats.fail_count >= 1);
    assert(stats.rx_timeout_count >= 1);

    free(mem);
    printf("  PASS: test_master_success_retry_and_fail\n");
}

static void test_master_rx_error_paths(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    start_master_rx_wait(pp);
    assert(ping_pong_on_rx_done(pp, packet, 3, 0, 0) == PING_PONG_ERR_INVALID_PARAM);
    assert(ping_pong_on_rx_done(pp, NULL, sizeof(packet), 0, 0) == PING_PONG_ERR_NULL_PTR);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[4] ^= 0x55u;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.crc_error_count == 1);

    assert(ping_pong_build_pong(packet, sizeof(packet), 99) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 1);

    assert(ping_pong_build_ping(packet, sizeof(packet), 0) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.conflict_count == 1);

    packet[0] = 0xAAu;
    packet[1] = packet[2] = packet[3] = packet[4] = packet[5] = 0;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_FAIL) == 0);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 2);

    free(mem);
    printf("  PASS: test_master_rx_error_paths\n");
}

static void test_slave_flow_and_error_paths(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t packet[PING_PONG_PACKET_SIZE];
    uint8_t expected[PING_PONG_PACKET_SIZE];
    ping_pong_stats_t stats;

    init_slave(pp, 100);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(find_last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);

    assert(ping_pong_build_ping(packet, sizeof(packet), 42) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 12) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(find_last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);
    assert(ping_pong_build_pong(expected, sizeof(expected), 42) == PING_PONG_OK);
    assert(memcmp(find_last_notify(PING_PONG_NOTIFY_TX_REQUEST)->payload.tx_request.tx_buffer,
                  expected,
                  PING_PONG_PACKET_SIZE) == 0);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_count == 1);
    assert(stats.last_rssi == -30);
    assert(stats.last_snr == 12);

    assert(ping_pong_build_ping(packet, sizeof(packet), 1) == PING_PONG_OK);
    packet[4] ^= 0x11u;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 12) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.crc_error_count == 1);

    assert(ping_pong_build_pong(packet, sizeof(packet), 1) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 12) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.conflict_count == 1);

    packet[0] = 0xFFu;
    packet[1] = packet[2] = packet[3] = packet[4] = packet[5] = 0;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 12) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 1);

    g_time_ms = 1000;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_timeout_count == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_build_ping(packet, sizeof(packet), 77) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -30, 12) == PING_PONG_OK);
    g_time_ms += 20;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.tx_timeout_count == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_slave(pp, 0);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    g_time_ms = 999999;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_timeout_count == 0);

    free(mem);
    printf("  PASS: test_slave_flow_and_error_paths\n");
}

static void test_added_stats_counters(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_stats_t stats;

    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    g_time_ms = 11;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.tx_timeout_count == 1);
    assert(stats.retry_count == 1);

    g_time_ms = 22;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.fail_count == 1);

    free(mem);
    printf("  PASS: test_added_stats_counters\n");
}

int main(void)
{
    printf("Running PingPong extension tests...\n");
    test_build_packet_helpers();
    test_init_and_config_errors();
    test_state_lifecycle_errors();
    test_tx_len_and_core_built_packet();
    test_master_success_retry_and_fail();
    test_master_rx_error_paths();
    test_slave_flow_and_error_paths();
    test_added_stats_counters();
    printf("All PingPong extension tests passed.\n");
    return 0;
}
