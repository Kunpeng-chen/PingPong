/*
 * PingPong 中间件实现 - 主从一体版
 */

#include "ping_pong.h"

/* 内部 memset 替代，避免引入 <string.h> */
static inline void pp_memset(void *dst, int val, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)val;
    }
}

/* ==================== 内部常量 ==================== */

#define PING_PONG_MAGIC 0x50494E47

/* 包类型 */
#define PACKET_TYPE_PING  0x01
#define PACKET_TYPE_PONG  0x02

/* 包头部 */
typedef struct {
    uint8_t type;
    uint8_t seq_hi;     /* 序列号高字节 */
    uint8_t seq_lo;     /* 序列号低字节 */
    uint8_t reserved;
    /* CRC-16 追加在包尾 */
} ping_pong_header_t;

/* CRC-16 大小 */
#define PING_PONG_CRC_SIZE 2

/* 内部最小包长度 = 头部 + CRC（与公开 PING_PONG_MIN_PACKET_SIZE 一致） */
#define PP_MIN_PACKET_SIZE (sizeof(ping_pong_header_t) + PING_PONG_CRC_SIZE)

/* parse_packet 返回码 */
#define PARSE_OK          0
#define PARSE_ERR_FORMAT (-1)
#define PARSE_ERR_CRC    (-2)

/* 大端字节序编解码 */
static inline uint16_t get_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/* 状态转换事件（仅包含需要查表守卫的事件） */
typedef enum {
    EVT_START_MASTER,
    EVT_START_SLAVE,
    EVT_STOP,
    EVT_TX_DONE,
    EVT_RX_DONE,
    EVT_COUNT
} pp_event_t;

/* 合法状态转换矩阵 [当前状态][事件] = 是否允许 */
static const uint8_t valid_transitions[4][EVT_COUNT] = {
    /* IDLE: */
    [PING_PONG_STATE_IDLE] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
    },
    /* TX: */
    [PING_PONG_STATE_TX] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 1, [EVT_RX_DONE] = 0,
    },
    /* RX_WAIT: */
    [PING_PONG_STATE_RX_WAIT] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 1,
    },
    /* STOPPED: */
    [PING_PONG_STATE_STOPPED] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
    },
};

/* 检查状态转换是否合法 */
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
    
    /* 端口 */
    ping_pong_port_t port;
    
    /* 运行参数 */
    uint16_t current_seq;
    uint16_t current_retry;
    uint32_t tx_start_time;
    uint32_t rx_start_time;
    
    /* 统计 */
    ping_pong_stats_t stats;
};

/* TX buffer immediately follows the struct in memory */
#define PP_TX_BUFFER(pp) ((uint8_t *)(pp) + sizeof(struct ping_pong))

/* ==================== 内部函数声明 ==================== */

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify);
static uint16_t compute_crc16(const uint8_t *data, uint32_t len);
static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type);
static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms,
                                  int16_t rssi, int16_t snr);
static void handle_master_fail(ping_pong_t *pp, uint32_t reason);
static void handle_master_retry(ping_pong_t *pp);
static void handle_slave_ping_received(ping_pong_t *pp, uint16_t seq,
                                        int16_t rssi, int16_t snr);
static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms);
static void send_tx_request(ping_pong_t *pp);

/* Trace helper macro - calls trace hook if set */
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

/* CRC-16 CCITT (0xFFFF initial, polynomial 0x1021) */
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
    if (!data || len < PP_MIN_PACKET_SIZE) {
        return PARSE_ERR_FORMAT;
    }
    
    /* Verify CRC over header portion */
    uint32_t header_len = len - PING_PONG_CRC_SIZE;
    uint16_t computed_crc = compute_crc16(data, header_len);
    uint16_t received_crc = get_u16_be(&data[header_len]);
    if (computed_crc != received_crc) {
        return PARSE_ERR_CRC;
    }

    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    
    if (header->type != expected_type) {
        return PARSE_ERR_FORMAT;
    }
    
    /* 16-bit 序列号比较 */
    uint16_t pkt_seq = get_u16_be(&header->seq_hi);
    if (pkt_seq != pp->current_seq) {
        return PARSE_ERR_FORMAT;
    }
    
    return PARSE_OK;
}

static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms,
                                  int16_t rssi, int16_t snr)
{
    PP_TRACE(pp, "master: success");
    pp->stats.success_count++;
    pp->stats.last_rtt_ms = rtt_ms;
    pp->stats.last_rssi = rssi;
    pp->stats.last_snr = snr;
    
    ping_pong_notify_t notify;
    pp_memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_SUCCESS;
    notify.timestamp_ms = pp->port.get_time_ms();
    notify.seq = pp->current_seq;
    notify.payload.success.rtt_ms = rtt_ms;
    notify.payload.success.rssi = rssi;
    notify.payload.success.snr = snr;
    send_notify(pp, &notify);
    
    pp->state = PING_PONG_STATE_IDLE;
}

static void handle_master_fail(ping_pong_t *pp, uint32_t reason)
{
    PP_TRACE(pp, "master: fail");
    pp->stats.fail_count++;
    
    ping_pong_notify_t notify;
    pp_memset(&notify, 0, sizeof(notify));
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
    
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    
    send_tx_request(pp);
}

static void handle_slave_ping_received(ping_pong_t *pp, uint16_t seq,
                                        int16_t rssi, int16_t snr)
{
    PP_TRACE(pp, "slave: ping received");
    pp->current_seq = seq;
    pp->stats.rx_count++;
    pp->stats.last_rssi = rssi;
    pp->stats.last_snr = snr;
    
    /* 回复 Pong */
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    
    send_tx_request(pp);
}

static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms)
{
    PP_TRACE(pp, "state: enter RX_WAIT");
    ping_pong_notify_t notify;
    pp_memset(&notify, 0, sizeof(notify));

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
    pp_memset(&tx_notify, 0, sizeof(tx_notify));
    tx_notify.type = PING_PONG_NOTIFY_TX_REQUEST;
    tx_notify.timestamp_ms = pp->tx_start_time;
    tx_notify.seq = pp->current_seq;
    tx_notify.payload.tx_request.tx_buffer = PP_TX_BUFFER(pp);
    tx_notify.payload.tx_request.tx_buffer_size = PING_PONG_TX_BUFFER_SIZE;

    pp->stats.tx_count++;

    send_notify(pp, &tx_notify);
}

/* ==================== 公开 API 实现 ==================== */

ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port)
{
    if (!pp || !port || !port->get_time_ms || !port->notify) {
        return PING_PONG_ERR_NULL_PTR;
    }
    
    pp_memset(pp, 0, sizeof(ping_pong_t));
    pp->magic = PING_PONG_MAGIC;
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->port = *port;
    
    /* 填充默认配置，可通过 set_config 覆盖 */
    pp->config.max_retries   = PING_PONG_DEFAULT_MAX_RETRIES;
    pp->config.rx_timeout_ms = PING_PONG_DEFAULT_RX_TIMEOUT_MS;
    pp->config.tx_timeout_ms = PING_PONG_DEFAULT_TX_TIMEOUT_MS;
    
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
    
    pp->config = *config;
    
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

    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }

    /* 按角色校验配置参数 */
    if (role == PING_PONG_ROLE_MASTER) {
        if (pp->config.rx_timeout_ms == 0 ||
            pp->config.rx_timeout_ms > PING_PONG_MAX_TIMEOUT_MS) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
        if (pp->config.max_retries == 0 ||
            pp->config.max_retries > PING_PONG_MAX_RETRIES) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
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
    
    pp_memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    
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

    /* TX 超时保护 */
    if (pp->state == PING_PONG_STATE_TX) {
        if (pp->config.tx_timeout_ms > 0) {
            if ((now_ms - pp->tx_start_time) >= pp->config.tx_timeout_ms) {
                if (pp->role == PING_PONG_ROLE_MASTER) {
                    if (pp->current_retry < pp->config.max_retries) {
                        pp->current_retry++;
                        handle_master_retry(pp);
                    } else {
                        handle_master_fail(pp, PING_PONG_FAIL_REASON_TX_TIMEOUT);
                    }
                } else {
                    /* Slave TX 超时，回退到 RX_WAIT 继续监听 */
                    PP_TRACE(pp, "slave: tx timeout, back to rx");
                    enter_rx_wait(pp, now_ms);
                }
            }
        }
        return PING_PONG_OK;
    }

    if (pp->state != PING_PONG_STATE_RX_WAIT) {
        return PING_PONG_OK;
    }
    
    uint32_t timeout_ms;
    
    if (pp->role == PING_PONG_ROLE_MASTER) {
        timeout_ms = pp->config.rx_timeout_ms;
        if ((now_ms - pp->tx_start_time) >= timeout_ms) {
            if (pp->current_retry < pp->config.max_retries) {
                pp->current_retry++;
                handle_master_retry(pp);
            } else {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_MAX_RETRIES);
            }
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (pp->config.rx_timeout_ms == 0) {
            return PING_PONG_OK;
        }
        timeout_ms = pp->config.rx_timeout_ms;
        if ((now_ms - pp->rx_start_time) >= timeout_ms) {
            ping_pong_notify_t notify;
            pp_memset(&notify, 0, sizeof(notify));
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
    
    if (len < PP_MIN_PACKET_SIZE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    
    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    /* 从包头提取 16-bit 序列号 */
    uint16_t seq = get_u16_be(&header->seq_hi);
    
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (header->type == PACKET_TYPE_PONG) {
            int rc = parse_packet(pp, data, len, PACKET_TYPE_PONG);
            if (rc == PARSE_OK) {
                uint32_t rtt_ms = pp->port.get_time_ms() - pp->tx_start_time;
                handle_master_success(pp, rtt_ms, rssi, snr);
            } else if (rc == PARSE_ERR_CRC) {
                /* CRC 校验失败 */
                handle_master_fail(pp, PING_PONG_FAIL_REASON_CRC_ERROR);
            } else {
                handle_master_fail(pp, PING_PONG_FAIL_REASON_PARSE_ERROR);
            }
        } else if (header->type == PACKET_TYPE_PING) {
            pp->stats.conflict_count++;
            handle_master_fail(pp, PING_PONG_FAIL_REASON_CONFLICT);
        } else {
            handle_master_fail(pp, PING_PONG_FAIL_REASON_PARSE_ERROR);
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (header->type == PACKET_TYPE_PING) {
            /* 接受前校验 CRC */
            uint32_t header_len = len - PING_PONG_CRC_SIZE;
            uint16_t computed = compute_crc16(data, header_len);
            uint16_t received = get_u16_be(&data[header_len]);
            if (computed != received) {
                enter_rx_wait(pp, pp->port.get_time_ms());
            } else {
                handle_slave_ping_received(pp, seq, rssi, snr);
            }
        } else if (header->type == PACKET_TYPE_PONG) {
            pp->stats.conflict_count++;
            enter_rx_wait(pp, pp->port.get_time_ms());
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

int ping_pong_is_valid(const ping_pong_t *pp)
{
    return (pp != NULL && pp->magic == PING_PONG_MAGIC) ? 1 : 0;
}

uint32_t ping_pong_instance_size(void)
{
    return (uint32_t)(sizeof(struct ping_pong) + PING_PONG_TX_BUFFER_SIZE);
}

/* Compile-time check that context fits within 256 bytes */
_Static_assert(sizeof(struct ping_pong) <= 256,
               "struct ping_pong exceeds 256-byte limit");

/* Compile-time check that TX buffer size is sufficient */
_Static_assert(PING_PONG_TX_BUFFER_SIZE >= PING_PONG_MIN_PACKET_SIZE,
               "PING_PONG_TX_BUFFER_SIZE must be >= PING_PONG_MIN_PACKET_SIZE");
