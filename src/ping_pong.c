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

/* 包头部 */
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t reserved[2];
} ping_pong_header_t;

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
static int parse_pong_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len);
static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms);
static void handle_master_fail(ping_pong_t *pp, uint32_t reason);
static void handle_master_retry(ping_pong_t *pp);
static void handle_slave_ping_received(ping_pong_t *pp, uint8_t seq);
static void handle_conflict(ping_pong_t *pp, uint32_t conflict_type);

/* ==================== 内部函数实现 ==================== */

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify)
{
    if (pp->port.notify) {
        pp->port.notify(pp, notify);
    }
}

static void update_stats(ping_pong_t *pp)
{
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_STATS_UPDATED;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
}

static int parse_pong_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len)
{
    if (!data || len < sizeof(ping_pong_header_t)) {
        return -1;
    }
    
    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    
    if (header->type != PACKET_TYPE_PONG) {
        return -1;
    }
    
    if (header->seq != (uint8_t)(pp->current_seq & 0xFF)) {
        return -1;
    }
    
    return 0;
}

static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms)
{
    pp->stats.success_count++;
    pp->stats.last_rtt_ms = rtt_ms;
    pp->stats.last_rssi = pp->last_rssi;
    pp->stats.last_snr = pp->last_snr;
    update_stats(pp);
    
    ping_pong_notify_t notify;
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
    pp->stats.fail_count++;
    update_stats(pp);
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_FAIL;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.fail.fail_reason = reason;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

static void handle_master_retry(ping_pong_t *pp)
{
    pp->stats.retry_count++;
    update_stats(pp);
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_RETRY;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.retry.retry_count = pp->current_retry;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    
    ping_pong_notify_t tx_notify;
    tx_notify.type = PING_PONG_NOTIFY_TX_REQUEST;
    tx_notify.timestamp_ms = pp->tx_start_time;
    tx_notify.seq = pp->current_seq;
    tx_notify.payload.tx_request.tx_buffer = pp->tx_buffer;
    tx_notify.payload.tx_request.tx_buffer_size = pp->config.tx_buffer_size;
    send_notify(pp, &tx_notify);
}

static void handle_slave_ping_received(ping_pong_t *pp, uint8_t seq)
{
    pp->stats.slave_rx_count++;
    pp->current_seq = seq;
    update_stats(pp);

    /* 通知装配层收到了 Ping */
    ping_pong_notify_t rx_notify;
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
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_TX_REQUEST;
    notify.timestamp_ms = pp->tx_start_time;
    notify.seq = pp->current_seq;
    notify.payload.tx_request.tx_buffer = pp->tx_buffer;
    notify.payload.tx_request.tx_buffer_size = pp->config.tx_buffer_size;
    send_notify(pp, &notify);
}

static void handle_conflict(ping_pong_t *pp, uint32_t conflict_type)
{
    pp->stats.conflict_count++;
    update_stats(pp);
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_CONFLICT;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.conflict.conflict_type = conflict_type;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

/* ==================== 公开 API 实现 ==================== */

int ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port)
{
    if (!pp || !port || !port->get_time_ms || !port->notify) {
        return -1;
    }
    
    memset(pp, 0, sizeof(ping_pong_t));
    pp->magic = PING_PONG_MAGIC;
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->port = *port;
    
    return 0;
}

int ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config)
{
    if (!pp || pp->magic != PING_PONG_MAGIC || !config) {
        return -1;
    }
    
    if (pp->state != PING_PONG_STATE_IDLE && pp->state != PING_PONG_STATE_STOPPED) {
        return -1;
    }
    
    if (config->timeout_ms == 0 || config->max_retries == 0 || config->tx_buffer_size == 0) {
        return -1;
    }
    
    pp->config = *config;
    pp->config_set = 1;
    pp->tx_buffer = (uint8_t*)pp + sizeof(ping_pong_t);
    
    return 0;
}

int ping_pong_start(ping_pong_t *pp, ping_pong_role_t role)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return -1;
    }
    
    if (!pp->config_set) {
        return -1;
    }
    
    if (pp->state != PING_PONG_STATE_IDLE && pp->state != PING_PONG_STATE_STOPPED) {
        return -1;
    }
    
    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) {
        return -1;
    }
    
    pp->role = role;
    pp->current_seq++;
    pp->current_retry = 0;
    
    if (role == PING_PONG_ROLE_MASTER) {
        pp->state = PING_PONG_STATE_TX;
        pp->tx_start_time = pp->port.get_time_ms();
        
        ping_pong_notify_t notify;
        notify.type = PING_PONG_NOTIFY_TX_REQUEST;
        notify.timestamp_ms = pp->tx_start_time;
        notify.seq = pp->current_seq;
        notify.payload.tx_request.tx_buffer = pp->tx_buffer;
        notify.payload.tx_request.tx_buffer_size = pp->config.tx_buffer_size;
        send_notify(pp, &notify);
    } else {
        pp->state = PING_PONG_STATE_RX_WAIT;
        pp->rx_start_time = pp->port.get_time_ms();
        
        ping_pong_notify_t notify;
        notify.type = PING_PONG_NOTIFY_RX_REQUEST;
        notify.timestamp_ms = pp->rx_start_time;
        notify.seq = 0;
        send_notify(pp, &notify);
    }
    
    ping_pong_notify_t started;
    started.type = PING_PONG_NOTIFY_STARTED;
    started.timestamp_ms = pp->port.get_time_ms();
    started.seq = pp->current_seq;
    send_notify(pp, &started);
    
    return 0;
}

int ping_pong_stop(ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return -1;
    }
    
    if (pp->state == PING_PONG_STATE_IDLE || pp->state == PING_PONG_STATE_STOPPED) {
        return -1;
    }
    
    pp->state = PING_PONG_STATE_STOPPED;
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_STOPPED;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
    
    return 0;
}

int ping_pong_reset(ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return -1;
    }
    
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->current_seq = 0;
    pp->current_retry = 0;
    pp->tx_start_time = 0;
    pp->rx_start_time = 0;
    
    memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_RESET;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = 0;
    send_notify(pp, &notify);
    
    return 0;
}

int ping_pong_process(ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return -1;
    }
    
    if (pp->state != PING_PONG_STATE_RX_WAIT) {
        return 0;
    }
    
    uint32_t now_ms = pp->port.get_time_ms();
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
            return 0;
        }
        timeout_ms = pp->config.slave_rx_timeout_ms;
        if ((now_ms - pp->rx_start_time) >= timeout_ms) {
            ping_pong_notify_t notify;
            notify.type = PING_PONG_NOTIFY_RX_TIMEOUT;
            notify.timestamp_ms = now_ms;
            notify.seq = pp->current_seq;
            send_notify(pp, &notify);
            pp->state = PING_PONG_STATE_IDLE;
        }
    }
    
    return 0;
}

int ping_pong_on_tx_done(ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) {
        return -1;
    }
    
    if (pp->state != PING_PONG_STATE_TX) {
        return -1;
    }
    
    pp->rx_start_time = pp->port.get_time_ms();
    pp->state = PING_PONG_STATE_RX_WAIT;
    
    ping_pong_notify_t notify;
    notify.type = PING_PONG_NOTIFY_RX_REQUEST;
    notify.timestamp_ms = pp->rx_start_time;
    notify.seq = pp->current_seq;
    send_notify(pp, &notify);
    
    return 0;
}

int ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len, int16_t rssi, int16_t snr)
{
    if (!pp || pp->magic != PING_PONG_MAGIC || !data) {
        return -1;
    }
    
    if (pp->state != PING_PONG_STATE_RX_WAIT) {
        return -1;
    }
    
    if (len < sizeof(ping_pong_header_t)) {
        return -1;
    }
    
    pp->last_rssi = rssi;
    pp->last_snr = snr;
    
    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    uint8_t seq = header->seq;
    
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (header->type == PACKET_TYPE_PONG) {
            if (parse_pong_packet(pp, data, len) == 0) {
                uint32_t rtt_ms = pp->port.get_time_ms() - pp->rx_start_time;
                pp->stats.master_tx_count++;
                handle_master_success(pp, rtt_ms);
            } else {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_PARSE_ERROR);
            }
        } else if (header->type == PACKET_TYPE_PING) {
            handle_conflict(pp, PING_PONG_CONFLICT_MASTER_RX_PING);
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (header->type == PACKET_TYPE_PING) {
            handle_slave_ping_received(pp, seq);
        } else if (header->type == PACKET_TYPE_PONG) {
            handle_conflict(pp, PING_PONG_CONFLICT_SLAVE_RX_PONG);
        }
    }
    
    return 0;
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

int ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats)
{
    if (!pp || pp->magic != PING_PONG_MAGIC || !stats) {
        return -1;
    }
    
    *stats = pp->stats;
    return 0;
}