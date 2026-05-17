/*
 * Focused runtime state transition regression tests.
 *
 * These tests protect the current external behavior before the runtime
 * state-dispatch refactor. They intentionally do not depend on internal
 * implementation details.
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

static ping_pong_config_t test_config(void)
{
    ping_pong_config_t config = {
        .max_retries = 1,
        .rx_timeout_ms = 100,
        .tx_timeout_ms = 0,
        .auto_restart = 0,
        .restart_delay_ms = 0,
    };
    return config;
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
    assert(ping_pong_set_config(pp, config) == PING_PONG_OK);
}

static void test_master_start_enters_tx(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();

    init_instance(pp, &config);
    g_time_ms = 1000;

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_MASTER);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);

    free(mem);
    printf("  PASS: test_master_start_enters_tx\n");
}

static void test_slave_start_enters_rx_wait(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();

    init_instance(pp, &config);
    g_time_ms = 1000;

    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_get_role(pp) == PING_PONG_ROLE_SLAVE);
    assert(count_notify(PING_PONG_NOTIFY_RX_REQUEST) == 1);

    free(mem);
    printf("  PASS: test_slave_start_enters_rx_wait\n");
}

static void test_tx_done_enters_rx_wait(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();

    init_instance(pp, &config);
    g_time_ms = 1000;

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);

    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(count_notify(PING_PONG_NOTIFY_RX_REQUEST) == 1);

    free(mem);
    printf("  PASS: test_tx_done_enters_rx_wait\n");
}

static void test_stop_from_tx_and_rx_wait(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();

    init_instance(pp, &config);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(ping_pong_stop(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_STOPPED);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_stop(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_STOPPED);

    free(mem);
    printf("  PASS: test_stop_from_tx_and_rx_wait\n");
}

static void test_rx_timeout_retries_master(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();
    ping_pong_stats_t stats;

    init_instance(pp, &config);
    g_time_ms = 1000;
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_RX_WAIT);

    g_time_ms = 1101;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 2);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count == 1);
    assert(stats.fail_count == 0);

    free(mem);
    printf("  PASS: test_rx_timeout_retries_master\n");
}

static void test_rx_timeout_max_retries_fails_master(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();
    ping_pong_stats_t stats;
    const ping_pong_notify_t *fail;

    init_instance(pp, &config);
    g_time_ms = 1000;
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);

    g_time_ms = 1101;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);
    g_time_ms = 1110;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);

    g_time_ms = 1202;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    fail = last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fail != NULL);
    assert(fail->payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count == 1);
    assert(stats.fail_count == 1);
    assert(stats.rx_timeout_count == 1);

    free(mem);
    printf("  PASS: test_rx_timeout_max_retries_fails_master\n");
}

static void test_invalid_state_events_are_rejected(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_config_t config = test_config();
    uint8_t packet[PING_PONG_PACKET_SIZE];

    init_instance(pp, &config);
    assert(ping_pong_stop(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_INVALID_STATE);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), 0, 0) == PING_PONG_ERR_INVALID_STATE);

    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_ERR_INVALID_STATE);
    assert(ping_pong_start(pp, PING_PONG_ROLE_SLAVE) == PING_PONG_ERR_INVALID_STATE);

    free(mem);
    printf("  PASS: test_invalid_state_events_are_rejected\n");
}

int main(void)
{
    printf("Running runtime state transition tests...\n");
    test_master_start_enters_tx();
    test_slave_start_enters_rx_wait();
    test_tx_done_enters_rx_wait();
    test_stop_from_tx_and_rx_wait();
    test_rx_timeout_retries_master();
    test_rx_timeout_max_retries_fails_master();
    test_invalid_state_events_are_rejected();
    printf("All runtime state transition tests passed.\n");
    return 0;
}
