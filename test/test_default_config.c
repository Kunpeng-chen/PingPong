/*
 * PingPong default configuration tests.
 */

#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CTX_EXTRA_BYTES 256u
#define MAX_NOTIFICATIONS   16

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

static void reset_recording(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    memset(g_notifications, 0, sizeof(g_notifications));
}

static uint8_t *alloc_instance(void)
{
    uint32_t size = ping_pong_instance_size() + TEST_CTX_EXTRA_BYTES;
    uint8_t *mem = (uint8_t *)calloc(1u, size);
    assert(mem != NULL);
    return mem;
}

static ping_pong_port_t make_port(void)
{
    ping_pong_port_t port = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify,
        .user_data = NULL,
        .trace = NULL,
    };
    return port;
}

static void test_get_default_config_values(void)
{
    ping_pong_config_t config = {0};

    ping_pong_get_default_config(NULL);
    ping_pong_get_default_config(&config);

    assert(config.max_retries == PING_PONG_DEFAULT_MAX_RETRIES);
    assert(config.rx_timeout_ms == PING_PONG_DEFAULT_RX_TIMEOUT_MS);
    assert(config.tx_timeout_ms == PING_PONG_DEFAULT_TX_TIMEOUT_MS);

    printf("  PASS: test_get_default_config_values\n");
}

static void test_init_uses_default_config_without_runtime_override(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_port_t port = make_port();

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(g_notify_count == 1);
    assert(g_notifications[0].type == PING_PONG_NOTIFY_TX_REQUEST);

    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = PING_PONG_DEFAULT_RX_TIMEOUT_MS;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(g_notify_count == 3);
    assert(g_notifications[2].type == PING_PONG_NOTIFY_TX_REQUEST);

    free(mem);
    printf("  PASS: test_init_uses_default_config_without_runtime_override\n");
}

static void test_runtime_config_overrides_default_config(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_port_t port = make_port();
    ping_pong_config_t config;
    ping_pong_stats_t stats;

    reset_recording();
    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    ping_pong_get_default_config(&config);
    config.max_retries = 1;
    config.rx_timeout_ms = 25;
    config.tx_timeout_ms = 0;
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);

    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);

    g_time_ms = 24;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(g_notify_count == 2);

    g_time_ms = 25;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(g_notify_count == 3);
    assert(g_notifications[2].type == PING_PONG_NOTIFY_TX_REQUEST);

    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = 50;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(g_notify_count == 5);
    assert(g_notifications[4].type == PING_PONG_NOTIFY_FAIL);
    assert(g_notifications[4].payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);

    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.retry_count == 1);
    assert(stats.fail_count == 1);

    free(mem);
    printf("  PASS: test_runtime_config_overrides_default_config\n");
}

static void test_local_modify_from_default_config(void)
{
    ping_pong_config_t config;

    ping_pong_get_default_config(&config);
    config.tx_timeout_ms = 0;

    assert(config.max_retries == PING_PONG_DEFAULT_MAX_RETRIES);
    assert(config.rx_timeout_ms == PING_PONG_DEFAULT_RX_TIMEOUT_MS);
    assert(config.tx_timeout_ms == 0);

    printf("  PASS: test_local_modify_from_default_config\n");
}

int main(void)
{
    printf("Running PingPong default config tests...\n");
    test_get_default_config_values();
    test_init_uses_default_config_without_runtime_override();
    test_runtime_config_overrides_default_config();
    test_local_modify_from_default_config();
    printf("All PingPong default config tests passed.\n");
    return 0;
}
