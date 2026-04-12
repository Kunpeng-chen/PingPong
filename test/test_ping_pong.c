/*
 * PingPong 中间件单元测试
 *
 * 纯 C assert 测试，覆盖：
 * - init/config/start/stop/reset 参数校验
 * - Master 完整流程（TX→RX→success/fail/retry）
 * - Slave 完整流程（RX→Ping→TX→RX）
 * - 冲突检测
 * - 边界条件
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

static void mock_notify(ping_pong_t *pp, const ping_pong_notify_t *notify)
{
    (void)pp;
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

static ping_pong_port_t g_port = {
    .get_time_ms = mock_get_time_ms,
    .notify = mock_notify,
};

static ping_pong_config_t g_default_config = {
    .timeout_ms = 100,
    .max_retries = 3,
    .tx_buffer_size = 64,
    .slave_rx_timeout_ms = 5000,
};

/* Build a Ping packet */
static void build_ping(uint8_t *buf, uint8_t seq)
{
    buf[0] = 0x01; /* PING */
    buf[1] = seq;
    buf[2] = 0;
    buf[3] = 0;
}

/* Build a Pong packet */
static void build_pong(uint8_t *buf, uint8_t seq)
{
    buf[0] = 0x02; /* PONG */
    buf[1] = seq;
    buf[2] = 0;
    buf[3] = 0;
}

/* Helper: init + config */
static void init_and_config(void)
{
    reset_test_state();
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    assert(ping_pong_init(g_pp, &g_port) == 0);
    assert(ping_pong_set_config(g_pp, &g_default_config) == 0);
}

/* ==================== 测试用例 ==================== */

/* --- Init tests --- */

static void test_init_null_params(void)
{
    reset_test_state();
    assert(ping_pong_init(NULL, &g_port) == -1);
    assert(ping_pong_init(g_pp, NULL) == -1);

    ping_pong_port_t bad_port = { .get_time_ms = NULL, .notify = mock_notify };
    assert(ping_pong_init(g_pp, &bad_port) == -1);

    bad_port.get_time_ms = mock_get_time_ms;
    bad_port.notify = NULL;
    assert(ping_pong_init(g_pp, &bad_port) == -1);

    printf("  PASS: test_init_null_params\n");
}

static void test_init_success(void)
{
    reset_test_state();
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    assert(ping_pong_init(g_pp, &g_port) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_NONE);
    printf("  PASS: test_init_success\n");
}

/* --- Config tests --- */

static void test_config_null_params(void)
{
    init_and_config();  /* re-init for magic */
    /* restore to init state for testing bad config */
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);

    assert(ping_pong_set_config(NULL, &g_default_config) == -1);
    assert(ping_pong_set_config(g_pp, NULL) == -1);

    printf("  PASS: test_config_null_params\n");
}

static void test_config_bad_values(void)
{
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);

    ping_pong_config_t bad = g_default_config;

    bad.timeout_ms = 0;
    assert(ping_pong_set_config(g_pp, &bad) == -1);
    bad = g_default_config;

    bad.max_retries = 0;
    assert(ping_pong_set_config(g_pp, &bad) == -1);
    bad = g_default_config;

    bad.tx_buffer_size = 0;
    assert(ping_pong_set_config(g_pp, &bad) == -1);

    printf("  PASS: test_config_bad_values\n");
}

/* --- Start tests --- */

static void test_start_without_config(void)
{
    memset(g_ctx_mem, 0, sizeof(g_ctx_mem));
    ping_pong_init(g_pp, &g_port);
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == -1);
    printf("  PASS: test_start_without_config\n");
}

static void test_start_invalid_role(void)
{
    init_and_config();
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_NONE) == -1);
    printf("  PASS: test_start_invalid_role\n");
}

static void test_start_master(void)
{
    init_and_config();
    g_time_ms = 1000;
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_MASTER);

    /* Should have TX_REQUEST and STARTED notifications */
    assert(find_last_notify(PING_PONG_NOTIFY_TX_REQUEST) != NULL);
    assert(find_last_notify(PING_PONG_NOTIFY_STARTED) != NULL);

    printf("  PASS: test_start_master\n");
}

static void test_start_slave(void)
{
    init_and_config();
    g_time_ms = 1000;
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_SLAVE);

    assert(find_last_notify(PING_PONG_NOTIFY_RX_REQUEST) != NULL);
    assert(find_last_notify(PING_PONG_NOTIFY_STARTED) != NULL);

    printf("  PASS: test_start_slave\n");
}

/* --- Stop tests --- */

static void test_stop_idle(void)
{
    init_and_config();
    assert(ping_pong_stop(g_pp) == -1); /* Can't stop IDLE */
    printf("  PASS: test_stop_idle\n");
}

static void test_stop_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_stop(g_pp) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_STOPPED);
    assert(find_last_notify(PING_PONG_NOTIFY_STOPPED) != NULL);
    printf("  PASS: test_stop_running\n");
}

/* --- Reset tests --- */

static void test_reset(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_reset(g_pp) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);
    assert(ping_pong_get_role(g_pp) == PING_PONG_ROLE_NONE);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.success_count == 0);
    assert(stats.fail_count == 0);
    assert(stats.retry_count == 0);

    printf("  PASS: test_reset\n");
}

/* --- Master success flow --- */

static void test_master_success_flow(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    /* TX completes → enters RX_WAIT */
    g_time_ms = 1010;
    assert(ping_pong_on_tx_done(g_pp) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    /* Receive matching Pong */
    uint8_t pong[4];
    build_pong(pong, 0);
    g_time_ms = 1050;
    assert(ping_pong_on_rx_done(g_pp, pong, 4, -50, 10) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    /* Verify success notification with RTT */
    const ping_pong_notify_t *sn = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(sn != NULL);
    /* RTT = 1050 - 1000 = 50 (from tx_start_time, not rx_start_time) */
    assert(sn->payload.success.rtt_ms == 50);
    assert(sn->payload.success.rssi == -50);
    assert(sn->payload.success.snr == 10);

    /* Verify stats */
    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.success_count == 1);
    assert(stats.master_tx_count == 1);
    assert(stats.last_rtt_ms == 50);

    printf("  PASS: test_master_success_flow\n");
}

/* --- Master retry + fail flow --- */

static void test_master_retry_and_fail(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    /* TX done */
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    /* Timeout triggers retry 1 */
    g_time_ms = 1110;  /* 100ms timeout from tx_start_time=1000 */
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 1);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    /* TX done, wait, timeout retry 2 */
    g_time_ms = 1120;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1230;
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 2);

    /* TX done, wait, timeout retry 3 */
    g_time_ms = 1240;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1350;
    ping_pong_process(g_pp);
    assert(count_notify(PING_PONG_NOTIFY_RETRY) == 3);

    /* TX done, wait, timeout → max retries reached → FAIL */
    g_time_ms = 1360;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1470;
    ping_pong_process(g_pp);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_MAX_RETRIES);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_IDLE);

    /* Verify stats */
    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.retry_count == 3);
    assert(stats.fail_count == 1);
    /* master_tx_count: 1 initial + 3 retries = 4 */
    assert(stats.master_tx_count == 4);

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

    /* Send Pong with wrong seq */
    uint8_t bad_pong[4];
    build_pong(bad_pong, 99);  /* Wrong seq */
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, bad_pong, 4, -60, 5);

    const ping_pong_notify_t *fn = find_last_notify(PING_PONG_NOTIFY_FAIL);
    assert(fn != NULL);
    assert(fn->payload.fail.fail_reason == PING_PONG_FAIL_REASON_PARSE_ERROR);

    printf("  PASS: test_master_parse_error\n");
}

/* --- Master receives unknown type (1.2) --- */

static void test_master_unknown_packet_type(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);

    uint8_t bad_packet[4] = { 0xFF, 0, 0, 0 };  /* Unknown type */
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, bad_packet, 4, -60, 5);

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

    uint8_t ping[4];
    build_ping(ping, 0);
    g_time_ms = 1020;
    ping_pong_on_rx_done(g_pp, ping, 4, -60, 5);

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

    /* Receive Ping */
    uint8_t ping[4];
    build_ping(ping, 42);
    g_time_ms = 2000;
    assert(ping_pong_on_rx_done(g_pp, ping, 4, -40, 8) == 0);

    /* Should notify PING_RECEIVED and then TX_REQUEST */
    assert(find_last_notify(PING_PONG_NOTIFY_PING_RECEIVED) != NULL);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_TX);

    /* TX done → back to RX_WAIT */
    g_time_ms = 2010;
    assert(ping_pong_on_tx_done(g_pp) == 0);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    /* Verify stats */
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

    uint8_t pong[4];
    build_pong(pong, 0);
    g_time_ms = 2000;
    ping_pong_on_rx_done(g_pp, pong, 4, -40, 8);

    const ping_pong_notify_t *cn = find_last_notify(PING_PONG_NOTIFY_CONFLICT);
    assert(cn != NULL);
    assert(cn->payload.conflict.conflict_type == PING_PONG_CONFLICT_SLAVE_RX_PONG);

    printf("  PASS: test_slave_conflict\n");
}

/* --- Slave unknown packet type (1.2) --- */

static void test_slave_unknown_packet_type(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    uint8_t bad_packet[4] = { 0xAA, 0, 0, 0 };
    g_time_ms = 2000;
    ping_pong_on_rx_done(g_pp, bad_packet, 4, -40, 8);

    /* Slave should resume RX_WAIT */
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_unknown_packet_type\n");
}

/* --- Slave RX timeout --- */

static void test_slave_rx_timeout(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    g_time_ms = 6001;  /* 5000ms timeout */
    ping_pong_process(g_pp);

    assert(find_last_notify(PING_PONG_NOTIFY_RX_TIMEOUT) != NULL);
    assert(ping_pong_get_state(g_pp) == PING_PONG_STATE_RX_WAIT);

    printf("  PASS: test_slave_rx_timeout\n");
}

/* --- Slave no timeout when slave_rx_timeout_ms == 0 --- */

static void test_slave_no_timeout(void)
{
    init_and_config();

    /* Reconfigure with 0 slave timeout */
    ping_pong_config_t config = g_default_config;
    config.slave_rx_timeout_ms = 0;
    ping_pong_set_config(g_pp, &config);

    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);

    g_time_ms = 999999;
    ping_pong_process(g_pp);

    /* No timeout should have fired */
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

    uint8_t pong[4];
    build_pong(pong, 0);
    g_time_ms = 1050;
    ping_pong_on_rx_done(g_pp, pong, 4, -50, 10);

    /* Restart as master - seq should increment */
    ping_pong_stop(g_pp);
    g_notify_count = 0;  /* clear for easier checking */
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
    uint8_t pong[4];
    build_pong(pong, 0);

    /* IDLE state */
    assert(ping_pong_on_rx_done(g_pp, pong, 4, -50, 10) == -1);

    /* TX state */
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_on_rx_done(g_pp, pong, 4, -50, 10) == -1);

    printf("  PASS: test_rx_done_wrong_state\n");
}

/* --- on_tx_done in wrong state --- */

static void test_tx_done_wrong_state(void)
{
    init_and_config();
    /* IDLE state */
    assert(ping_pong_on_tx_done(g_pp) == -1);

    /* RX_WAIT state */
    ping_pong_start(g_pp, PING_PONG_ROLE_SLAVE);
    assert(ping_pong_on_tx_done(g_pp) == -1);

    printf("  PASS: test_tx_done_wrong_state\n");
}

/* --- on_rx_done with short data --- */

static void test_rx_done_short_data(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    ping_pong_on_tx_done(g_pp);

    uint8_t short_data[2] = { 0x02, 0x00 };
    assert(ping_pong_on_rx_done(g_pp, short_data, 2, -50, 10) == -1);

    printf("  PASS: test_rx_done_short_data\n");
}

/* --- get_stats null params --- */

static void test_get_stats_null(void)
{
    init_and_config();
    ping_pong_stats_t stats;
    assert(ping_pong_get_stats(NULL, &stats) == -1);
    assert(ping_pong_get_stats(g_pp, NULL) == -1);
    printf("  PASS: test_get_stats_null\n");
}

/* --- process in non-RX_WAIT state does nothing --- */

static void test_process_noop(void)
{
    init_and_config();
    /* IDLE */
    assert(ping_pong_process(g_pp) == 0);

    /* TX state */
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_process(g_pp) == 0);

    printf("  PASS: test_process_noop\n");
}

/* --- RTT calculated from tx_start_time (1.4 verification) --- */

static void test_rtt_from_tx_start(void)
{
    init_and_config();
    g_time_ms = 100;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    /* tx_start_time = 100 */

    g_time_ms = 120;
    ping_pong_on_tx_done(g_pp);
    /* rx_start_time = 120 */

    uint8_t pong[4];
    build_pong(pong, 0);
    g_time_ms = 150;
    ping_pong_on_rx_done(g_pp, pong, 4, -50, 10);

    /* RTT should be 150 - 100 = 50 (from tx_start) not 150 - 120 = 30 (from rx_start) */
    const ping_pong_notify_t *sn = find_last_notify(PING_PONG_NOTIFY_SUCCESS);
    assert(sn != NULL);
    assert(sn->payload.success.rtt_ms == 50);

    printf("  PASS: test_rtt_from_tx_start\n");
}

/* --- master_tx_count increments on every TX (1.5 verification) --- */

static void test_master_tx_count_on_every_tx(void)
{
    init_and_config();
    g_time_ms = 1000;
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);

    ping_pong_stats_t stats;
    ping_pong_get_stats(g_pp, &stats);
    assert(stats.master_tx_count == 1);  /* First TX on start */

    /* TX done, timeout, retry */
    g_time_ms = 1010;
    ping_pong_on_tx_done(g_pp);
    g_time_ms = 1110;
    ping_pong_process(g_pp);  /* Retry → TX_REQUEST → tx_count++ */

    ping_pong_get_stats(g_pp, &stats);
    assert(stats.master_tx_count == 2);

    printf("  PASS: test_master_tx_count_on_every_tx\n");
}

/* --- Config not allowed while running --- */

static void test_config_while_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_set_config(g_pp, &g_default_config) == -1);
    printf("  PASS: test_config_while_running\n");
}

/* --- Start not allowed while running --- */

static void test_start_while_running(void)
{
    init_and_config();
    ping_pong_start(g_pp, PING_PONG_ROLE_MASTER);
    assert(ping_pong_start(g_pp, PING_PONG_ROLE_MASTER) == -1);
    printf("  PASS: test_start_while_running\n");
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

    printf("\n=== ALL %d TESTS PASSED ===\n", 28);
    return 0;
}
