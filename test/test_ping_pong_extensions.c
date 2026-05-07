/*
 * PingPong extension tests for tx_len and added statistics.
 */

/*============================ INCLUDES ======================================*/

#include "ping_pong.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*============================ MACROS ========================================*/

#define TEST_CTX_EXTRA_BYTES 256

/*============================ MACROFIED FUNCTIONS ===========================*/

/* None. */

/*============================ TYPES =========================================*/

/* None. */

/*============================ GLOBAL VARIABLES ==============================*/

/* None. */

/*============================ LOCAL VARIABLES ===============================*/

static uint32_t g_time_ms;
static ping_pong_notify_t g_last_notify;
static int g_notify_count;

/*============================ PROTOTYPES ====================================*/

static uint32_t mock_get_time_ms(void);
static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                        void *user_data);
static uint8_t *alloc_instance(void);
static void init_master(ping_pong_t *pp);
static void test_tx_len_and_core_built_packet(void);
static void test_added_stats_counters(void);

/*============================ IMPLEMENTATION ================================*/

static uint32_t mock_get_time_ms(void)
{
    return g_time_ms;
}

static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                        void *user_data)
{
    (void)pp;
    (void)user_data;
    g_last_notify = *notify;
    g_notify_count++;
}

static uint8_t *alloc_instance(void)
{
    uint32_t size = ping_pong_instance_size() + TEST_CTX_EXTRA_BYTES;
    uint8_t *mem = (uint8_t *)calloc(1, size);
    assert(mem != NULL);
    return mem;
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
        .tx_timeout_ms = 10,
    };

    g_time_ms = 0;
    g_notify_count = 0;
    memset(&g_last_notify, 0, sizeof(g_last_notify));

    assert(ping_pong_init(pp, &port) == PING_PONG_OK);
    assert(ping_pong_set_config(pp, &config) == PING_PONG_OK);
}

static void test_tx_len_and_core_built_packet(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    uint8_t expected[PING_PONG_PACKET_SIZE];

    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);

    assert(g_notify_count == 1);
    assert(g_last_notify.type == PING_PONG_NOTIFY_TX_REQUEST);
    assert(g_last_notify.payload.tx_request.tx_len == PING_PONG_PACKET_SIZE);
    assert(g_last_notify.payload.tx_request.tx_buffer_size == PING_PONG_TX_BUFFER_SIZE);
    assert(g_last_notify.payload.tx_request.tx_buffer != NULL);

    assert(ping_pong_build_ping(expected, sizeof(expected), 0) == PING_PONG_OK);
    assert(memcmp(g_last_notify.payload.tx_request.tx_buffer,
                  expected,
                  PING_PONG_PACKET_SIZE) == 0);

    free(mem);
    printf("  PASS: test_tx_len_and_core_built_packet\n");
}

static void test_added_stats_counters(void)
{
    uint8_t *mem = alloc_instance();
    ping_pong_t *pp = (ping_pong_t *)mem;
    ping_pong_stats_t stats;
    uint8_t packet[PING_PONG_PACKET_SIZE];

    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);

    assert(ping_pong_build_pong(packet, sizeof(packet), 0) == PING_PONG_OK);
    packet[4] ^= 0x55;
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.crc_error_count == 1);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);

    assert(ping_pong_build_pong(packet, sizeof(packet), 99) == PING_PONG_OK);
    assert(ping_pong_on_rx_done(pp, packet, sizeof(packet), -40, 7) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.parse_error_count == 1);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    g_time_ms = 11;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.tx_timeout_count == 1);

    assert(ping_pong_reset(pp) == PING_PONG_OK);
    init_master(pp);
    assert(ping_pong_start(pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = 250;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_on_tx_done(pp) == PING_PONG_OK);
    g_time_ms = 500;
    assert(ping_pong_process(pp) == PING_PONG_OK);
    assert(ping_pong_get_stats(pp, &stats) == PING_PONG_OK);
    assert(stats.rx_timeout_count == 1);

    free(mem);
    printf("  PASS: test_added_stats_counters\n");
}

int main(void)
{
    printf("Running PingPong extension tests...\n");
    test_tx_len_and_core_built_packet();
    test_added_stats_counters();
    printf("All PingPong extension tests passed.\n");
    return 0;
}
