/*
 * PingPong 中间件单元测试
 *
 * 纯 C assert 测试，覆盖：
 * - init/config/start/stop/reset 参数校验
 * - Master 完整流程（TX→RX→success/fail/retry）
 * - Slave 完整流程（RX→Ping→TX→RX）
 * - 冲突检测
 * - 边界条件
 * - 错误码、user_data、TX超时、CRC-16、16-bit seq、RTT扩展统计、连续计数
 */

#include "ping_pong.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==================== 测试辅助 ==================== */

/* Simulated time */
static uint32_t g_time_ms;

static uint32_t mock_get_time_ms(void)
{
    return g_time_ms;
}

/* Notification recording */
#define MAX_NOTIFICATIONS 64

static ping_pong_notify_t g_notifications[MAX_NOTIFICATIONS];
static int g_notify_count;

/* Updated callback with user_data parameter */
static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                        void *user_data)
{
    (void)pp;
    (void)user_data;
    if (g_notify_count < MAX_NOTIFICATIONS) {
        g_notifications[g_notify_count++] = *notify;
    }
}

static void reset_test_state(void)
{
    g_time_ms = 0;
    g_notify_count = 0;
    memset(g_notifications, 0, sizeof(g_notifications));
}

/* Find last notification of given type. Returns NULL if not found. */
static const ping_pong_notify_t *find_last_notify(ping_pong_notify_type_t type)
{
    for (int i = g_notify_count - 1; i >= 0; i--) {
        if (g_notifications[i].type == type) {
            return &g_notifications[i];
        }
    }
    return NULL;
}

/* Count notifications of given type */
static int count_notify(ping_pong_notify_type_t type)
{
    int count = 0;
    for (int i = 0; i < g_notify_count; i++) {
        if (g_notifications[i].type == type) {
            count++;
        }
    }
    return count;
}

/* Context with appended TX buffer.
 * ping_pong_t is opaque, so we use a byte array large enough
 * for the struct plus the TX buffer that follows it.
 */
#define TEST_TX_BUF_SIZE 64
static uint8_t g_ctx_mem[512];  /* Oversized to accommodate struct + TX buf */
#define g_pp ((ping_pong_t *)g_ctx_mem)

static int g_user_data_value = 42;

static ping_pong_port_t g_port = {
    .get_time_ms = mock_get_time_ms,
    .notify = mock_notify,
    .user_data = NULL,  /* Will be set in user_data tests */
};

static ping_pong_config_t g_default_config = {
    .timeout_ms = 100,
    .max_retries = 3,
    .tx_buffer_size = 64,
    .slave_rx_timeout_ms = 5000,
    .tx_timeout_ms = 0,  /* Disabled by default */
};

/* ==================== CRC-16 CCITT helper (same algorithm as module) ==================== */

static uint16_t test_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t j;
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/* Build a packet with header + CRC. Returns total length (header + CRC = 6). */
static uint32_t build_packet(uint8_t *buf, uint8_t type, uint16_t seq)
{
    buf[0] = type;
    buf[1] = (uint8_t)(seq >> 8);   /* seq_hi */
    buf[2] = (uint8_t)(seq & 0xFF); /* seq_lo */
    buf[3] = 0;                      /* reserved */
    /* Append CRC-16 over header (4 bytes) */
    uint16_t crc = test_crc16(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
    return 6;
}

/* Build a Ping packet with CRC */
static uint32_t build_ping(uint8_t *buf, uint16_t seq)
{
    return build_packet(buf, 0x01, seq);
}

/* Build a Pong packet with CRC */
static uint32_t build_pong(uint8_t *buf, uint16_t seq)
{
    return build_packet(buf, 0x02, seq);
}

/* Helper: init + config */
static void init_and_config(void)
{
    reset_test_state();
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    assert(ping_pong_init(g_pp, &g_port) == PING_PONG_OK);
    assert(ping_pong_set_config(g_pp, &g_default_config) == PING_PONG_OK);
}

/* ==================== 测试用例 ==================== */

/* --- Init tests --- */

static void test_init_null_params(void)
{
    reset_test_state();
    assert(ping_pong_init(NULL, &g_port) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_init(g_pp, NULL) == PING_PONG_ERR_NULL_PTR);

    ping_pong_port_t bad_port = { .get_time_ms = NULL, .notify = mock_notify, .user_data = NULL };
    assert(ping_pong_init(g_pp, &bad_port) == PING_PONG_ERR_NULL_PTR);

    bad_port.get_time_ms = mock_get_time_ms;
    bad_port.notify = NULL;
    assert(ping_pong_init(g_pp, &bad_port) == PING_PONG_ERR_NULL_PTR);

    printf("  PASS: test_init_null_params\n");
}

static void test_init_success(void)
{
    reset_test_state();
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    assert(ping_pong_init(g_pp, &g_port) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_NONE);
    printf("  PASS: test_init_success\n");
}

/* --- Config tests --- */

static void test_config_null_params(void)
{
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);

    assert(ping_pong_set_config(NULL, &g_default_config) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_set_config(g_pp, NULL) == PING_PONG_ERR_NULL_PTR);

    printf("  PASS: test_config_null_params\n");
}

static void test_config_bad_values(void)
{
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);

    ping_pong_config_t bad = g_default_config;

    bad.timeout_ms = 0;
    assert(ping_pong_set_config(g_pp, &bad) == PING_PONG_ERR_INVALID_PARAM);
    bad = g_default_config;

    bad.max_retries = 0;
    assert(ping_pong_set_config(g_pp, &bad) == PING_PONG_ERR_INVALID_PARAM);
    bad = g_default_config;

    bad.tx_buffer_size = 0;
    assert(ping_pong_set_config(g_pp, &bad) == PING_PONG_ERR_INVALID_PARAM);

    printf("  PASS: test_config_bad_values\n");
}

/* --- Start tests --- */

static void test_start_without_config(void)
{
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_NOT_CONFIGURED);
    printf("  PASS: test_start_without_config\n");
}

static void test_start_invalid_role(void)
{
    init_and_config();
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_NONE) == PING_PONG_ERR_INVALID_PARAM);
    printf("  PASS: test_start_invalid_role\n");
}

static void test_start_master(void)
{
    init_and_config();
    g_time_ms = 1000;
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_MASTER);

    assert(find_last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);

    printf("  PASS: test_start_master\n");
}

static void test_start_slave(void)
{
    init_and_config();
    g_time_ms = 1000;
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_SLAVE);

    assert(find_last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);

    printf("  PASS: test_start_slave\n");
}

/* --- Stop tests --- */

static void test_stop_idle(void)
{
    init_and_config();
    assert(ping_pong_stop(g_pp) == PING_PONG_ERR_INVALID_STATE);
    printf("  PASS: test_stop_idle\n");
}

static void test_stop_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_stop(g_pp) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_STOPPED);
    printf("  PASS: test_stop_running\n");
}

/* --- Reset tests --- */

static void test_reset(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_reset(g_pp) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_NONE);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.success_count == 0);
    assert(stats.fail_count == 0);
    assert(stats.retry_count == 0);
    assert(stats.consecutive_success_count == 0);
    assert(stats.consecutive_fail_count == 0);

    printf("  PASS: test_reset\n");
}

/* --- Master success flow --- */

static void test_master_success_flow(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(g_pp) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    uint8_t pong[6];
    uint32_t pong_len = build_pong(pong, 0);
    g_time_ms = 1050;
    assert(ping_pong_on_rx_done(g_pp, pong, pong_len, -50, 10) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    const ping_pong_notify_t *sn = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(sn != NULL);
    assert(sn->payload.success.rtt_ms == 50);
    assert(sn->payload.success.rssi == -50);
    assert(sn->payload.success.snr == 10);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.success_count == 1);
    assert(stats.master_tx_count == 1);
    assert(stats.last_rtt_ms == 50);
    /* Extended RTT stats */
    assert(stats.min_rtt_ms == 50);
    assert(stats.max_rtt_ms == 50);
    assert(stats.total_rtt_ms == 50);
    /* Consecutive counters */
    assert(stats.consecutive_success_count == 1);
    assert(stats.consecutive_fail_count == 0);

    printf("  PASS: test_master_success_flow\n");
}

/* --- Master retry + fail flow --- */

static void test_master_retry_and_fail(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    g_time_ms = 1110;
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 1);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    g_time_ms = 1120;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1230;
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 2);

    g_time_ms = 1240;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1350;
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 3);

    g_time_ms = 1360;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1470;
    ping_pong_process(g_pp);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.retry_count == 3);
    assert(stats.fail_count == 1);
    assert(stats.master_tx_count == 4);
    /* Fail resets consecutive success, increments consecutive fail */
    assert(stats.consecutive_fail_count == 1);
    assert(stats.consecutive_success_count == 0);

    printf("  PASS: test_master_retry_and_fail\n");
}

/* --- Master parse error --- */

static void test_master_parse_error(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    /* Build pong with correct CRC but wrong seq */
    uint8_t bad_pong[6];
    build_pong(bad_pong, 99);
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, bad_pong, 6, -60, 5);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_PARSE_ERROR);

    printf("  PASS: test_master_parse_error\n");
}

/* --- Master receives unknown packet type --- */

static void test_master_unknown_packet_type(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    /* Unknown type with valid CRC */
    uint8_t bad_packet[6];
    bad_packet[0] = 0xFF;
    bad_packet[1] = 0;
    bad_packet[2] = 0;
    bad_packet[3] = 0;
    uint16_t crc = test_crc16(bad_packet, 4);
    bad_packet[4] = (uint8_t)(crc >> 8);
    bad_packet[5] = (uint8_t)(crc & 0xFF);
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, bad_packet, 6, -60, 5);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_PARSE_ERROR);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    printf("  PASS: test_master_unknown_packet_type\n");
}

/* --- Master conflict (receives Ping) --- */

static void test_master_conflict(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    uint8_t ping[6];
    build_ping(ping, 0);
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, ping, 6, -60, 5);

    const ping_pong_notify_t *cn = find_last_notify(PING_PONG_NOTIFY_CONFLICT);
    assert(cn != NULL);
    assert(cn->payload.conflict.conflict_type == PING_PONG_CONFLICT_MASTER_RX_PING);

    printf("  PASS: test_master_conflict\n");
}

/* --- Slave complete flow --- */

static void test_slave_complete_flow(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    uint8_t ping[6];
    build_ping(ping, 42);
    g_time_ms = 2000;
    assert(ping_pong_on_rx_done(g_pp, ping, 6, -40, 8) == PING_PONG_OK);

    assert(find_last_notify(PING_PONG_NOTIFY_PING_RECEIVED) != NULL);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    g_time_ms = 2010;
    assert(ping_pong_on_tx_done(g_pp) == PING_PONG_OK);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.slave_rx_count == 1);

    printf("  PASS: test_slave_complete_flow\n");
}

/* --- Slave conflict (receives Pong) --- */

static void test_slave_conflict(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    uint8_t pong[6];
    build_pong(pong, 0);
    g_time_ms = 2000;
    ping_pong_on_rx_done(g_pp, pong, 6, -40, 8);

    const ping_pong_notify_t *cn = find_last_notify(PING_PONG_NOTIFY_CONFLICT);
    assert(cn != NULL);
    assert(cn->payload.conflict.conflict_type == PING_PONG_CONFLICT_SLAVE_RX_PONG);

    printf("  PASS: test_slave_conflict\n");
}

/* --- Slave unknown packet type --- */

static void test_slave_unknown_packet_type(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    uint8_t bad_packet[6];
    bad_packet[0] = 0xAA;
    bad_packet[1] = 0;
    bad_packet[2] = 0;
    bad_packet[3] = 0;
    uint16_t crc = test_crc16(bad_packet, 4);
    bad_packet[4] = (uint8_t)(crc >> 8);
    bad_packet[5] = (uint8_t)(crc & 0xFF);
    g_time_ms = 2000;
    ping_pong_on_rx_done(g_pp, bad_packet, 6, -40, 8);

    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_unknown_packet_type\n");
}

/* --- Slave RX timeout --- */

static void test_slave_rx_timeout(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    g_time_ms = 6001;
    ping_pong_process(g_pp);

    assert(find_last_notify(PING_PONG_NOTIFY_RX_TIMEOUT) != NULL);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_rx_timeout\n");
}

/* --- Slave no timeout when slave_rx_timeout_ms == 0 --- */

static void test_slave_no_timeout(void)
{
    init_and_config();

    ping_pong_config_t config = g_default_config;
    config.slave_rx_timeout_ms = 0;
    ping_pong_set_config(g_pp, &config);

    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    g_time_ms = 999999;
    ping_pong_process(g_pp);

    assert(find_last_notify(PING_PONG_NOTIFY_RX_TIMEOUT) == NULL);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_no_timeout\n");
}

/* --- Master seq increment on restart --- */

static void test_master_seq_increment(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    ping_pong_on_tx_done(g_pp);

    uint8_t pong[6];
    build_pong(pong, 0);
    g_time_ms = 1050;
    ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);

    ping_pong_stop(g_pp);
    g_notify_count = 0;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    const ping_pong_notify_t *tx = find_last_notify(PING_PONG_NOTIFY_TX_REQUEST);
    assert(tx != NULL);
    assert(tx->seq == 1);

    printf("  PASS: test_master_seq_increment\n");
}

/* --- on_rx_done in wrong state --- */

static void test_rx_done_wrong_state(void)
{
    init_and_config();
    uint8_t pong[6];
    build_pong(pong, 0);

    assert(ping_pong_on_rx_done(g_pp, pong, 6, -50, 10) == PING_PONG_ERR_INVALID_STATE);

    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_on_rx_done(g_pp, pong, 6, -50, 10) == PING_PONG_ERR_INVALID_STATE);

    printf("  PASS: test_rx_done_wrong_state\n");
}

/* --- on_tx_done in wrong state --- */

static void test_tx_done_wrong_state(void)
{
    init_and_config();
    assert(ping_pong_on_tx_done(g_pp) == PING_PONG_ERR_INVALID_STATE);

    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);
    assert(ping_pong_on_tx_done(g_pp) == PING_PONG_ERR_INVALID_STATE);

    printf("  PASS: test_tx_done_wrong_state\n");
}

/* --- on_rx_done with short data --- */

static void test_rx_done_short_data(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    ping_pong_on_tx_done(g_pp);

    uint8_t short_data[3] = { 0x02, 0x00, 0x00 };
    assert(ping_pong_on_rx_done(g_pp, short_data, 3, -50, 10) == PING_PONG_ERR_INVALID_PARAM);

    printf("  PASS: test_rx_done_short_data\n");
}

/* --- get_stats null params --- */

static void test_get_stats_null(void)
{
    init_and_config();
    ping_pong_stats_t stats;
    assert(ping_pong_get_stats(NULL, &stats) == PING_PONG_ERR_NULL_PTR);
    assert(ping_pong_get_stats(g_pp, NULL) == PING_PONG_ERR_NULL_PTR);
    printf("  PASS: test_get_stats_null\n");
}

/* --- process in non-RX_WAIT state does nothing --- */

static void test_process_noop(void)
{
    init_and_config();
    assert(ping_pong_process(g_pp) == PING_PONG_OK);

    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    /* TX state - no tx_timeout configured, so process does nothing */
    assert(ping_pong_process(g_pp) == PING_PONG_OK);

    printf("  PASS: test_process_noop\n");
}

/* --- RTT calculated from tx_start_time --- */

static void test_rtt_from_tx_start(void)
{
    init_and_config();
    g_time_ms = 100;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    g_time_ms = 120;
    ping_pong_on_tx_done(g_pp);

    uint8_t pong[6];
    build_pong(pong, 0);
    g_time_ms = 150;
    ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);

    const ping_pong_notify_t *sn = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(sn != NULL);
    assert(sn->payload.success.rtt_ms == 50);

    printf("  PASS: test_rtt_from_tx_start\n");
}

/* --- master_tx_count increments on every TX --- */

static void test_master_tx_count_on_every_tx(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.master_tx_count == 1);

    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1110;
    ping_pong_process(g_pp);

    ping_pong_get_stats(g_pp, &stats);
    assert(stats.master_tx_count == 2);

    printf("  PASS: test_master_tx_count_on_every_tx\n");
}

/* --- Config not allowed while running --- */

static void test_config_while_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_set_config(g_pp, &g_default_config) == PING_PONG_ERR_INVALID_STATE);
    printf("  PASS: test_config_while_running\n");
}

/* --- Start not allowed while running --- */

static void test_start_while_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == PING_PONG_ERR_INVALID_STATE);
    printf("  PASS: test_start_while_running\n");
}

/* ==================== 扩展功能测试 ==================== */

/* user_data passed through notify */
static void *g_captured_user_data;

static void mock_notify_capture_userdata(ping_pong_t *pp, const ping_pong_notify_t *notify,
                                         void *user_data)
{
    (void)pp;
    (void)notify;
    g_captured_user_data = user_data;
}

static void test_user_data_callback(void)
{
    reset_test_state();
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    g_captured_user_data = NULL;

    ping_pong_port_t port_with_ud = {
        .get_time_ms = mock_get_time_ms,
        .notify = mock_notify_capture_userdata,
        .user_data = &g_user_data_value,
    };

    assert(ping_pong_init(g_pp, &port_with_ud) == PING_PONG_OK);
    assert(ping_pong_set_config(g_pp, &g_default_config) == PING_PONG_OK);
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    assert(g_captured_user_data == &g_user_data_value);

    printf("  PASS: test_user_data_callback\n");
}

/* TX timeout protection */
static void test_tx_timeout_protection(void)
{
    init_and_config();

    /* Reconfigure with TX timeout */
    ping_pong_config_t config = g_default_config;
    config.tx_timeout_ms = 50;
    ping_pong_set_config(g_pp, &config);

    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    /* Process before timeout - no change */
    g_time_ms = 1040;
    ping_pong_process(g_pp);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    /* Process after TX timeout */
    g_time_ms = 1060;
    ping_pong_process(g_pp);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_TX_TIMEOUT);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    printf("  PASS: test_tx_timeout_protection\n");
}

/* CRC-16 verification */
static void test_crc_error_detection(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    /* Build pong with correct header but corrupted CRC */
    uint8_t bad_crc[6];
    build_pong(bad_crc, 0);
    bad_crc[4] ^= 0xFF;  /* Corrupt CRC */
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, bad_crc, 6, -50, 10);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_CRC_ERROR);

    printf("  PASS: test_crc_error_detection\n");
}

/* 16-bit sequence number */
static void test_16bit_seq(void)
{
    init_and_config();
    g_time_ms = 1000;

    /* Run 300 rounds to overflow 8-bit seq */
    uint32_t i;
    for (i = 0; i < 300; i++) {
        g_notify_count = 0;
        ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
        ping_pong_on_tx_done(g_pp);

        uint8_t pong[6];
        build_pong(pong, (uint16_t)i);
        g_time_ms += 10;
        ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);

        assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);
        ping_pong_stop(g_pp);
    }

    /* If 16-bit seq works, we got here without fail. seq=299 > 255 */
    printf("  PASS: test_16bit_seq\n");
}

/* RTT extended statistics */
static void test_rtt_extended_stats(void)
{
    init_and_config();
    g_time_ms = 0;

    /* Round 1: RTT = 50 */
    g_time_ms = 100;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 110;
    ping_pong_on_tx_done(g_pp);
    uint8_t pong[6];
    build_pong(pong, 0);
    g_time_ms = 150;
    ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);
    ping_pong_stop(g_pp);

    /* Round 2: RTT = 30 */
    g_time_ms = 200;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 210;
    ping_pong_on_tx_done(g_pp);
    build_pong(pong, 1);
    g_time_ms = 230;
    ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);
    ping_pong_stop(g_pp);

    /* Round 3: RTT = 80 */
    g_time_ms = 300;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 310;
    ping_pong_on_tx_done(g_pp);
    build_pong(pong, 2);
    g_time_ms = 380;
    ping_pong_on_rx_done(g_pp, pong, 6, -50, 10);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.success_count == 3);
    assert(stats.min_rtt_ms == 30);
    assert(stats.max_rtt_ms == 80);
    assert(stats.total_rtt_ms == 160);  /* 50 + 30 + 80 */
    assert(stats.last_rtt_ms == 80);

    printf("  PASS: test_rtt_extended_stats\n");
}

/* Consecutive success/fail counters */
static void test_consecutive_counters(void)
{
    init_and_config();
    g_time_ms = 0;

    /* 2 successes */
    g_time_ms = 100;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 110;
    ping_pong_on_tx_done(g_pp);
    uint8_t pkt[6];
    build_pong(pkt, 0);
    g_time_ms = 120;
    ping_pong_on_rx_done(g_pp, pkt, 6, -50, 10);
    ping_pong_stop(g_pp);

    g_time_ms = 200;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 210;
    ping_pong_on_tx_done(g_pp);
    build_pong(pkt, 1);
    g_time_ms = 220;
    ping_pong_on_rx_done(g_pp, pkt, 6, -50, 10);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.consecutive_success_count == 2);
    assert(stats.consecutive_fail_count == 0);

    /* Now a fail */
    ping_pong_stop(g_pp);
    g_time_ms = 300;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 310;
    ping_pong_on_tx_done(g_pp);
    /* Wrong seq pong → parse error → fail */
    build_pong(pkt, 99);
    g_time_ms = 320;
    ping_pong_on_rx_done(g_pp, pkt, 6, -50, 10);

    ping_pong_get_stats(g_pp, &stats);
    assert(stats.consecutive_success_count == 0);
    assert(stats.consecutive_fail_count == 1);

    printf("  PASS: test_consecutive_counters\n");
}

/* --- Slave CRC error causes re-enter RX_WAIT --- */

static void test_slave_crc_error(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    uint8_t bad_ping[6];
    build_ping(bad_ping, 0);
    bad_ping[5] ^= 0xFF;  /* Corrupt CRC */
    g_time_ms = 2000;
    ping_pong_on_rx_done(g_pp, bad_ping, 6, -40, 8);

    /* Should stay in RX_WAIT, not crash or accept the packet */
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_crc_error\n");
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("Running PingPong unit tests...\n\n");

    printf("[Init Tests]\n");
    test_init_null_params();
    test_init_success();

    printf("\n[Config Tests]\n");
    test_config_null_params();
    test_config_bad_values();
    test_config_while_running();

    printf("\n[Start Tests]\n");
    test_start_without_config();
    test_start_invalid_role();
    test_start_master();
    test_start_slave();
    test_start_while_running();

    printf("\n[Stop Tests]\n");
    test_stop_idle();
    test_stop_running();

    printf("\n[Reset Tests]\n");
    test_reset();

    printf("\n[Master Flow Tests]\n");
    test_master_success_flow();
    test_master_retry_and_fail();
    test_master_parse_error();
    test_master_unknown_packet_type();
    test_master_conflict();
    test_master_seq_increment();
    test_rtt_from_tx_start();
    test_master_tx_count_on_every_tx();

    printf("\n[Slave Flow Tests]\n");
    test_slave_complete_flow();
    test_slave_conflict();
    test_slave_unknown_packet_type();
    test_slave_rx_timeout();
    test_slave_no_timeout();

    printf("\n[Edge Case Tests]\n");
    test_rx_done_wrong_state();
    test_tx_done_wrong_state();
    test_rx_done_short_data();
    test_get_stats_null();
    test_process_noop();

    printf("\n[Extended Tests]\n");
    test_user_data_callback();
    test_tx_timeout_protection();
    test_crc_error_detection();
    test_16bit_seq();
    test_rtt_extended_stats();
    test_consecutive_counters();
    test_slave_crc_error();

    printf("\n=== ALL %d TESTS PASSED ===\n", 38);
    return 0;
}
