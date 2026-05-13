#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTIFICATIONS 64

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

static void init_with_config(ping_pong_t *pp, ping_pong_config_t config)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
}

static ping_pong_config_t make_config(uint8_t auto_restart, uint32_t delay_ms)
{
    ping_pong_config_t config;
    ping_pong_get_default_config(&config);
    config.max_retries = 1;
    config.rx_timeout_ms = 100;
    config.tx_timeout_ms = 10;
    config.auto_restart = auto_restart;
    config.restart_delay_ms = delay_ms;
    return config;
}

static void complete_success_round(ping_pong_t *pp)
{
    uint8_t pong[PING_PONG_PACKET_SIZE];
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);
    g_time_ms = 10;
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    assert(ping_pong_build_pong(pong, sizeof(pong), 0) == PING_PONG_OK);
    g_time_ms = 20;
    assert(ping_pong_on_rx_done(pp, pong, sizeof(pong), -40, 8) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_SUCCESS) != NULL);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
}

static void test_default_auto_restart_off(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;

    init_with_config(pp, make_config(0u, 0u));
    complete_success_round(pp);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);
    g_time_ms = 20;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);

    free(mem);
    printf("  PASS: test_default_auto_restart_off\n");
}

static void test_auto_restart_after_success_without_reentry(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_stats_t stats;

    init_with_config(pp, make_config(1u, 0u));
    complete_success_round(pp);

    /* SUCCESS 回调期间不重入；下一轮 TX 要等后续 process()。 */
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.consecutive_fail_count == 0);

    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 2);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->seq == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);

    free(mem);
    printf("  PASS: test_auto_restart_after_success_without_reentry\n");
}

static void test_auto_restart_after_fail_and_failure_stats(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_stats_t stats;

    init_with_config(pp, make_config(1u, 0u));
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);

    g_time_ms = 11;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 2);

    g_time_ms = 22;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(last_notify(PING_PONG_NOTIFY_FAIL) != NULL);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.consecutive_fail_count == 1);
    assert(stats.last_fail_reason == PING_PONG_FAIL_REASON_TX_TIMEOUT);
    assert(stats.last_fail_timestamp_ms == 22);

    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 3);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->seq == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_TX);

    free(mem);
    printf("  PASS: test_auto_restart_after_fail_and_failure_stats\n");
}

static void test_auto_restart_delay(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;

    init_with_config(pp, make_config(1u, 50u));
    complete_success_round(pp);

    g_time_ms = 69;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 1);
    assert(ping_pong_get_state(pp) == PING_PONG_STATE_IDLE);

    g_time_ms = 70;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(count_notify(PING_PONG_NOTIFY_TX_REQUEST) == 2);
    assert(last_notify(PING_PONG_NOTIFY_TX_REQUEST)->seq == 1);

    free(mem);
    printf("  PASS: test_auto_restart_delay\n");
}

int main(void)
{
    printf("Running PingPong master auto-restart tests...\n");
    test_default_auto_restart_off();
    test_auto_restart_after_success_without_reentry();
    test_auto_restart_after_fail_and_failure_stats();
    test_auto_restart_delay();
    printf("All PingPong master auto-restart tests passed.\n");
    return 0;
}
