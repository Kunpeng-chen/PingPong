/*
 * PingPong 中间件实现 - 主从一体版
 */

#include "ping_pong.h"
#include <string.h>

/* ==================== 内部常量 ==================== */

#define PING_PONG_MAGIC 0x50494E47

/* 包类型 */
#define PACKET_TYPE_PING  0x01
#define PACKET_TYPE_PONG  0x02

/* 3.5: 包头部 - seq 扩展为 uint16_t（大端编码） */
typedef struct {
    uint8_t type;
    uint8_t seq_hi;     /* 序列号高字节 */
    uint8_t seq_lo;     /* 序列号低字节 */
    uint8_t reserved;
    /* 3.4: CRC-16 追加在包尾，不在头部中 */
} ping_pong_header_t;

/* CRC-16 大小 */
#define PING_PONG_CRC_SIZE 2

/* 最小包长度 = 头部 + CRC */
#define PING_PONG_MIN_PACKET_SIZE (sizeof(ping_pong_header_t) + PING_PONG_CRC_SIZE)

/* 4.5: 大端字节序编解码 */
static inline uint16_t get_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/* 4.2: 状态转换事件 */
typedef enum {
    EVT_START_MASTER,
    EVT_START_SLAVE,
    EVT_STOP,
    EVT_RESET,
    EVT_TX_DONE,
    EVT_RX_DONE,
    EVT_TIMEOUT,
    EVT_COUNT
} pp_event_t;

/* 4.2: Check if a state transition is valid */
/* 4.2: 合法状态转换矩阵 [当前状态][事件] = 是否允许 */
static const uint8_t valid_transitions[4][EVT_COUNT] = {
    /* IDLE: */
    [PING_PONG_STATE_IDLE] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_RESET] = 1,
        [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0, [EVT_TIMEOUT] = 0,
    },
    /* TX: */
    [PING_PONG_STATE_TX] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_RESET] = 1,
        [EVT_TX_DONE] = 1, [EVT_RX_DONE] = 0, [EVT_TIMEOUT] = 1,
    },
    /* RX_WAIT: */
    [PING_PONG_STATE_RX_WAIT] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_RESET] = 1,
        [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 1, [EVT_TIMEOUT] = 1,
    },
    /* STOPPED: */
    [PING_PONG_STATE_STOPPED] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_RESET] = 1,
        [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0, [EVT_TIMEOUT] = 0,
    },
};

/* 4.2: Check if a state transition is valid */
static int is_valid_transition(ping_pong_state_t state, pp_event_t event)
{
    if ((unsigned)state >= 4 || (unsigned)event >= EVT_COUNT) {
        return 0;
    }
    return valid_transitions[state][event];
}

/* ==================== 内部结构体 ==================== */

struct ping_pong {
    uint32_t magic;
    
    /* 状态和角色 */
    ping_pong_state_t state;
    ping_pong_role_t role;
    
    /* 配置 */
    ping_pong_config_t config;
    uint8_t config_set;
    
    /* 端口 */
    ping_pong_port_t port;
    
    /* 运行参数 */
    uint32_t current_seq;
    uint32_t current_retry;
    uint32_t tx_start_time;
    uint32_t rx_start_time;
    
    /* 统计 */
    ping_pong_stats_t stats;
    
    /* 最近接收信息 */
    int16_t last_rssi;
    int16_t last_snr;
    
    /* 发送缓冲区 */
    uint8_t *tx_buffer;
};

/* ==================== 内部函数声明 ==================== */

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify);
static void update_stats(ping_pong_t *pp);
static uint16_t compute_crc16(const uint8_t *data, uint32_t len);
static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type);
static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms);
static void handle_master_fail(ping_pong_t *pp, uint32_t reason);
static void handle_master_retry(ping_pong_t *pp);
static void handle_slave_ping_received(ping_pong_t *pp, uint16_t seq);
static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms);
static void send_tx_request(ping_pong_t *pp);
static void handle_conflict(ping_pong_t *pp, uint32_t conflict_type);

/* 4.1: Trace helper macro - calls trace hook if set */
#define PP_TRACE(pp, msg) do { \
    if ((pp)->port.trace) { (pp)->port.trace(msg); } \
} while (0)

/* ==================== 内部函数实现 ==================== */

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify)
{
    if (pp->port.notify) {
        pp->port.notify(pp, notify, pp->port.user_data);
    }
}

static void update_stats(ping_pong_t *pp)
{
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_STATS_UPDATED;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
}

/* 3.4: CRC-16 CCITT (0xFFFF initial, polynomial 0x1021) */
static uint16_t compute_crc16(const uint8_t *data, uint32_t len)
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

/* Parse and validate a received packet (type + seq + CRC) */
static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type)
{
    if (!data || len < PING_PONG_MIN_PACKET_SIZE) {
        return -1;
    }
    
    /* 3.4: Verify CRC over header portion */
    uint32_t header_len = len - PING_PONG_CRC_SIZE;
    uint16_t computed_crc = compute_crc16(data, header_len);
    uint16_t received_crc = get_u16_be(&data[header_len]);
    if (computed_crc != received_crc) {
        return -2; /* CRC error */
    }

    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    
    if (header->type != expected_type) {
        return -1;
    }
    
    /* 3.5: 16-bit sequence number comparison */
    uint16_t pkt_seq = get_u16_be(&header->seq_hi);
    if (pkt_seq != (uint16_t)(pp->current_seq & 0xFFFF)) {
        return -1;
    }
    
    return 0;
}

static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms)
{
    PP_TRACE(pp, "master: success");
    pp->stats.success_count++;
    pp->stats.last_rtt_ms = rtt_ms;
    pp->stats.last_rssi = pp->last_rssi;
    pp->stats.last_snr = pp->last_snr;

    /* 3.6: RTT extended stats */
    pp->stats.total_rtt_ms += rtt_ms;
    if (pp->stats.success_count == 1 || rtt_ms < pp->stats.min_rtt_ms) {
        pp->stats.min_rtt_ms = rtt_ms;
    }
    if (rtt_ms > pp->stats.max_rtt_ms) {
        pp->stats.max_rtt_ms = rtt_ms;
    }

    /* 3.7: Consecutive counters */
    pp->stats.consecutive_success_count++;
    pp->stats.consecutive_fail_count = 0;

    update_stats(pp);
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_SUCCESS;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.success.rtt_ms = rtt_ms;
    notify.payload.success.rssi = pp->last_rssi;
    notify.payload.success.snr = pp->last_snr;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

static void handle_master_fail(ping_pong_t *pp, uint32_t reason)
{
    PP_TRACE(pp, "master: fail");
    pp->stats.fail_count++;

    /* 3.7: Consecutive counters */
    pp->stats.consecutive_fail_count++;
    pp->stats.consecutive_success_count = 0;

    update_stats(pp);
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_FAIL;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.fail.fail_reason = reason;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

static void handle_master_retry(ping_pong_t *pp)
{
    PP_TRACE(pp, "master: retry");
    pp->stats.retry_count++;
    update_stats(pp);
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_RETRY;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.retry.retry_count = pp->current_retry;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    
    send_tx_request(pp);
}

static void handle_slave_ping_received(ping_pong_t *pp, uint16_t seq)
{
    PP_TRACE(pp, "slave: ping received");
    pp->current_seq = seq;
    pp->stats.slave_rx_count++;
    update_stats(pp);

    /* 通知装配层收到了 Ping */
    ping_pong_notify_t rx_notify;
    memset(&rx_notify, 0, sizeof(rx_notify));
    rx_notify.type = PING_PONG_NOTIFY_PING_RECEIVED;
    rx_notify.timestamp_ms = pp->port.get_time_ms();
    rx_notify.seq = seq;
    rx_notify.payload.ping_received.seq = seq;
    rx_notify.payload.ping_received.rssi = pp->last_rssi;
    rx_notify.payload.ping_received.snr = pp->last_snr;
    send_notify(pp, &rx_notify);
    
    /* 回复 Pong */
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    
    send_tx_request(pp);
}

static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms)
{
    PP_TRACE(pp, "state: enter RX_WAIT");
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));

    pp->state = PING_PONG_STATE_RX_WAIT;
    pp->rx_start_time = timestamp_ms;
    notify.type = PING_PONG_NOTIFY_RX_REQUEST;
    notify.timestamp_ms = timestamp_ms;
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
}

static void send_tx_request(ping_pong_t *pp)
{
    ping_pong_notify_t tx_notify;
    memset(&tx_notify, 0, sizeof(tx_notify));
    tx_notify.type = PING_PONG_NOTIFY_TX_REQUEST;
    tx_notify.timestamp_ms = pp->tx_start_time;
    tx_notify.seq = pp->current_seq;
    tx_notify.payload.tx_request.tx_buffer = pp->tx_buffer;
    tx_notify.payload.tx_request.tx_buffer_size = pp->config.tx_buffer_size;

    if (pp->role == PING_PONG_ROLE_MASTER) {
        pp->stats.master_tx_count++;
    }

    send_notify(pp, &tx_notify);
}

static void handle_conflict(ping_pong_t *pp, uint32_t conflict_type)
{
    PP_TRACE(pp, "conflict detected");
    pp->stats.conflict_count++;
    update_stats(pp);
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_CONFLICT;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.conflict.conflict_type = conflict_type;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

/* ==================== 公开 API 实现 ==================== */

ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port)
{
    if (!pp || !port || !port->get_time_ms || !port->notify) {
        return PING_PONG_ERR_NULL_PTR;
    }
    
    memset(pp, 0, sizeof(ping_pong_t));
    pp->magic = PING_PONG_MAGIC;
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->port = *port;
    
    PP_TRACE(pp, "init: ok");
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config)
{
    if (!pp || !config) {
        return PING_PONG_ERR_NULL_PTR;
    }
    
    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    if (pp->state != PING_PONG_STATE_IDLE && pp->state != PING_PONG_STATE_STOPPED) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    
    if (config->timeout_ms == 0 || config->max_retries == 0 || config->tx_buffer_size == 0) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    
    pp->config = *config;
    pp->config_set = 1;
    pp->tx_buffer = (uint8_t*)pp + sizeof(ping_pong_t);
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role)
{
    ping_pong_role_t previous_role;

    if (!pp) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    if (!pp->config_set) {
        return PING_PONG_ERR_NOT_CONFIGURED;
    }

    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }

    pp_event_t evt = (role == PING_PONG_ROLE_MASTER) ? EVT_START_MASTER : EVT_START_SLAVE;
    if (!is_valid_transition(pp->state, evt)) {
        return PING_PONG_ERR_INVALID_STATE;
    }

    previous_role = pp->role;
    if (role == PING_PONG_ROLE_MASTER && previous_role == PING_PONG_ROLE_MASTER) {
        pp->current_seq++;
    }

    pp->role = role;
    pp->current_retry = 0;
    
    if (role == PING_PONG_ROLE_MASTER) {
        pp->state = PING_PONG_STATE_TX;
        pp->tx_start_time = pp->port.get_time_ms();
        
        send_tx_request(pp);
    } else {
        enter_rx_wait(pp, pp->port.get_time_ms());
    }
    
    ping_pong_notify_t started;
    memset(&started, 0, sizeof(started));
    started.type = PING_PONG_NOTIFY_STARTED;
    started.timestamp_ms = pp->port.get_time_ms();
    started.seq = pp->current_seq;
    send_notify(pp, &started);
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_stop(ping_pong_t *pp)
{
    if (!pp) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    if (!is_valid_transition(pp->state, EVT_STOP)) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    
    pp->state = PING_PONG_STATE_STOPPED;
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_STOPPED;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_reset(ping_pong_t *pp)
{
    if (!pp) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->current_seq = 0;
    pp->current_retry = 0;
    pp->tx_start_time = 0;
    pp->rx_start_time = 0;
    
    memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    
    ping_pong_notify_t notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_RESET;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = 0;
    send_notify(pp, &notify);
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_process(ping_pong_t *pp)
{
    if (!pp) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    uint32_t now_ms = pp->port.get_time_ms();

    /* 3.3: TX timeout protection */
    if (pp->state == PING_PONG_STATE_TX) {
        if (pp->config.tx_timeout_ms > 0) {
            if ((now_ms - pp->tx_start_time) >= pp->config.tx_timeout_ms) {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_TX_TIMEOUT);
            }
        }
        return PING_PONG_OK;
    }

    if (pp->state != PING_PONG_STATE_RX_WAIT) {
        return PING_PONG_OK;
    }
    
    uint32_t timeout_ms;
    
    if (pp->role == PING_PONG_ROLE_MASTER) {
        timeout_ms = pp->config.timeout_ms;
        if ((now_ms - pp->tx_start_time) >= timeout_ms) {
            if (pp->current_retry < pp->config.max_retries) {
                pp->current_retry++;
                handle_master_retry(pp);
            } else {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_MAX_RETRIES);
            }
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (pp->config.slave_rx_timeout_ms == 0) {
            return PING_PONG_OK;
        }
        timeout_ms = pp->config.slave_rx_timeout_ms;
        if ((now_ms - pp->rx_start_time) >= timeout_ms) {
            ping_pong_notify_t notify;
            memset(&notify, 0, sizeof(notify));
            notify.type = PING_PONG_NOTIFY_RX_TIMEOUT;
            notify.timestamp_ms = now_ms;
            notify.seq = pp->current_seq;
            send_notify(pp, &notify);
            enter_rx_wait(pp, now_ms);
        }
    }
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp)
{
    if (!pp) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    if (!is_valid_transition(pp->state, EVT_TX_DONE)) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    
    enter_rx_wait(pp, pp->port.get_time_ms());
    
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr)
{
    if (!pp || !data) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    if (!is_valid_transition(pp->state, EVT_RX_DONE)) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    
    if (len < PING_PONG_MIN_PACKET_SIZE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    
    pp->last_rssi = rssi;
    pp->last_snr = snr;
    
    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    /* 3.5: 16-bit seq from packet header */
    uint16_t seq = get_u16_be(&header->seq_hi);
    
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (header->type == PACKET_TYPE_PONG) {
            int rc = parse_packet(pp, data, len, PACKET_TYPE_PONG);
            if (rc == 0) {
                uint32_t rtt_ms = pp->port.get_time_ms() - pp->tx_start_time;
                handle_master_success(pp, rtt_ms);
            } else if (rc == -2) {
                /* 3.4: CRC error */
                handle_master_fail(pp, PING_PONG_FAIL_REASON_CRC_ERROR);
            } else {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_PARSE_ERROR);
            }
        } else if (header->type == PACKET_TYPE_PING) {
            handle_conflict(pp, PING_PONG_CONFLICT_MASTER_RX_PING);
        } else {
            handle_master_fail(pp, PING_PONG_FAIL_REASON_PARSE_ERROR);
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (header->type == PACKET_TYPE_PING) {
            /* 3.4: Verify CRC before accepting */
            uint32_t header_len = len - PING_PONG_CRC_SIZE;
            uint16_t computed = compute_crc16(data, header_len);
            uint16_t received = get_u16_be(&data[header_len]);
            if (computed != received) {
                enter_rx_wait(pp, pp->port.get_time_ms());
            } else {
                handle_slave_ping_received(pp, seq);
            }
        } else if (header->type == PACKET_TYPE_PONG) {
            handle_conflict(pp, PING_PONG_CONFLICT_SLAVE_RX_PONG);
        } else {
            enter_rx_wait(pp, pp->port.get_time_ms());
        }
    }
    
    return PING_PONG_OK;
}

ping_pong_state_t ping_pong_get_state(const ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_STATE_IDLE;
    }
    return pp->state;
}

ping_pong_role_t ping_pong_get_role(const ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ROLE_NONE;
    }
    return pp->role;
}

ping_pong_err_t ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats)
{
    if (!pp || !stats) {
        return PING_PONG_ERR_NULL_PTR;
    }

    if (pp->magic != PING_PONG_MAGIC) {
        return PING_PONG_ERR_NOT_INITIALIZED;
    }
    
    *stats = pp->stats;
    return PING_PONG_OK;
}

/* Compile-time check that context fits within 256 bytes */
_Static_assert(sizeof(struct ping_pong) <= 256,
               "struct ping_pong exceeds 256-byte limit");
