/*
 * PingPong 中间件实现 - 主从一体版
 */

#include "ping_pong.h"

#define PING_PONG_MAGIC 0x50494E47
#define PACKET_TYPE_PING  0x01u
#define PACKET_TYPE_PONG  0x02u
#define PACKET_VERSION_V2 ((uint8_t)PING_PONG_PROTOCOL_VERSION_V2)
#define PING_PONG_CRC_SIZE 2u
#define V2_HEADER_CRC_OFFSET 22u
#define PARSE_OK          0
#define PARSE_ERR_FORMAT (-1)
#define PARSE_ERR_CRC    (-2)

#define PP_TX_BUFFER(pp) ((uint8_t *)(pp) + sizeof(struct ping_pong))

#define PP_TRACE(pp, msg) do { \
    if ((pp)->port.trace) { (pp)->port.trace(msg); } \
} while (0)

typedef enum {
    EVT_START_MASTER,
    EVT_START_SLAVE,
    EVT_STOP,
    EVT_TX_DONE,
    EVT_RX_DONE,
    EVT_COUNT
} pp_event_t;

struct ping_pong {
    uint32_t magic;
    ping_pong_state_t state;
    ping_pong_role_t role;
    ping_pong_config_t config;
    ping_pong_port_t port;
    uint16_t current_seq;
    uint16_t current_retry;
    uint32_t tx_start_time;
    uint32_t rx_start_time;
    uint8_t auto_restart_pending;
    uint32_t next_start_time;
    ping_pong_stats_t stats;
};

static void send_tx_request(ping_pong_t *pp);
static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms);
static void handle_master_retry(ping_pong_t *pp);

static const uint8_t valid_transitions[4][EVT_COUNT] = {
    [PING_PONG_STATE_IDLE] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
    },
    [PING_PONG_STATE_TX] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 1, [EVT_RX_DONE] = 0,
    },
    [PING_PONG_STATE_RX_WAIT] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 1,
    },
    [PING_PONG_STATE_STOPPED] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
    },
};

static inline void pp_memset(void *dst, int val, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)val;
    }
}

static inline uint16_t get_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static inline uint32_t get_u32_be(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3]);
}

static inline void put_u16_be(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value >> 8);
    buf[1] = (uint8_t)(value & 0xFFu);
}

static inline void put_u32_be(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)(value & 0xFFu);
}

static int is_valid_transition(ping_pong_state_t state, pp_event_t event)
{
    if ((unsigned)state >= 4u || (unsigned)event >= EVT_COUNT) {
        return 0;
    }
    return valid_transitions[state][event];
}

static int time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return ((uint32_t)(now_ms - target_ms) < 0x80000000u) ? 1 : 0;
}

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify)
{
    if (pp->port.notify) {
        pp->port.notify(pp, notify, pp->port.user_data);
    }
}

static uint16_t compute_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t j;
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8u; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static int is_v2_packet(const uint8_t *data, uint32_t len)
{
    return (data != NULL && len >= PING_PONG_V2_PACKET_SIZE && data[0] == PACKET_VERSION_V2) ? 1 : 0;
}

static uint8_t packet_type(const uint8_t *data, uint32_t len)
{
    if (is_v2_packet(data, len)) {
        return data[1];
    }
    return data[0];
}

static uint16_t packet_seq(const uint8_t *data, uint32_t len)
{
    if (is_v2_packet(data, len)) {
        return get_u16_be(&data[2]);
    }
    return get_u16_be(&data[1]);
}

static ping_pong_err_t build_v1_packet(uint8_t *buf, uint32_t buf_size,
                                       uint8_t type, uint16_t seq)
{
    uint16_t crc;
    if (!buf) {
        return PING_PONG_ERR_NULL_PTR;
    }
    if (buf_size < PING_PONG_V1_PACKET_SIZE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    buf[0] = type;
    put_u16_be(&buf[1], seq);
    buf[3] = 0u;
    crc = compute_crc16(buf, 4u);
    put_u16_be(&buf[4], crc);
    return PING_PONG_OK;
}

static ping_pong_err_t build_v2_packet(uint8_t *buf, uint32_t buf_size,
                                       uint8_t type, uint16_t seq,
                                       uint32_t network_id, uint16_t src_id,
                                       uint16_t dst_id)
{
    uint16_t crc;
    if (!buf) {
        return PING_PONG_ERR_NULL_PTR;
    }
    if (buf_size < PING_PONG_V2_PACKET_SIZE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    pp_memset(buf, 0, PING_PONG_V2_PACKET_SIZE);
    buf[0] = PACKET_VERSION_V2;
    buf[1] = type;
    put_u16_be(&buf[2], seq);
    put_u32_be(&buf[4], network_id);
    put_u16_be(&buf[8], src_id);
    put_u16_be(&buf[10], dst_id);
    crc = compute_crc16(buf, V2_HEADER_CRC_OFFSET);
    put_u16_be(&buf[V2_HEADER_CRC_OFFSET], crc);
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_build_ping(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    return ping_pong_build_ping_ex(buf, buf_size, seq,
                                   PING_PONG_DEFAULT_NETWORK_ID,
                                   PING_PONG_DEFAULT_SRC_ID,
                                   PING_PONG_DEFAULT_DST_ID);
}

ping_pong_err_t ping_pong_build_pong(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    return ping_pong_build_pong_ex(buf, buf_size, seq,
                                   PING_PONG_DEFAULT_NETWORK_ID,
                                   PING_PONG_DEFAULT_SRC_ID,
                                   PING_PONG_DEFAULT_DST_ID);
}

ping_pong_err_t ping_pong_build_ping_ex(uint8_t *buf, uint32_t buf_size, uint16_t seq,
                                        uint32_t network_id, uint16_t src_id,
                                        uint16_t dst_id)
{
    return build_v2_packet(buf, buf_size, PACKET_TYPE_PING, seq,
                           network_id, src_id, dst_id);
}

ping_pong_err_t ping_pong_build_pong_ex(uint8_t *buf, uint32_t buf_size, uint16_t seq,
                                        uint32_t network_id, uint16_t src_id,
                                        uint16_t dst_id)
{
    return build_v2_packet(buf, buf_size, PACKET_TYPE_PONG, seq,
                           network_id, src_id, dst_id);
}

static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type, uint8_t check_seq)
{
    uint32_t crc_offset;
    uint16_t computed_crc;
    uint16_t received_crc;

    if (!data || len < PING_PONG_MIN_PACKET_SIZE) {
        return PARSE_ERR_FORMAT;
    }

    if (is_v2_packet(data, len)) {
        crc_offset = V2_HEADER_CRC_OFFSET;
        computed_crc = compute_crc16(data, crc_offset);
        received_crc = get_u16_be(&data[crc_offset]);
        if (computed_crc != received_crc) {
            return PARSE_ERR_CRC;
        }
        if (data[1] != expected_type) {
            return PARSE_ERR_FORMAT;
        }
        if (get_u32_be(&data[4]) != pp->config.network_id) {
            return PARSE_ERR_FORMAT;
        }
        if (get_u16_be(&data[10]) != pp->config.src_id) {
            return PARSE_ERR_FORMAT;
        }
        if (get_u16_be(&data[8]) != pp->config.dst_id) {
            return PARSE_ERR_FORMAT;
        }
        if (check_seq && get_u16_be(&data[2]) != pp->current_seq) {
            return PARSE_ERR_FORMAT;
        }
        return PARSE_OK;
    }

    if (len < PING_PONG_V1_PACKET_SIZE) {
        return PARSE_ERR_FORMAT;
    }
    computed_crc = compute_crc16(data, 4u);
    received_crc = get_u16_be(&data[4]);
    if (computed_crc != received_crc) {
        return PARSE_ERR_CRC;
    }
    if (data[0] != expected_type) {
        return PARSE_ERR_FORMAT;
    }
    if (check_seq && get_u16_be(&data[1]) != pp->current_seq) {
        return PARSE_ERR_FORMAT;
    }
    return PARSE_OK;
}

static void schedule_master_auto_restart(ping_pong_t *pp, uint32_t now_ms)
{
    if (pp->role == PING_PONG_ROLE_MASTER && pp->config.auto_restart) {
        pp->auto_restart_pending = 1u;
        pp->next_start_time = now_ms + pp->config.restart_delay_ms;
    }
}

static void handle_master_success(ping_pong_t *pp, uint32_t rtt_ms,
                                  int16_t rssi, int16_t snr)
{
    uint32_t now_ms;
    ping_pong_notify_t notify;
    PP_TRACE(pp, "master: success");
    now_ms = pp->port.get_time_ms();
    pp->stats.success_count++;
    pp->stats.consecutive_fail_count = 0;
    pp->stats.last_rtt_ms = rtt_ms;
    pp->stats.last_rssi = rssi;
    pp->stats.last_snr = snr;
    pp_memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_SUCCESS;
    notify.timestamp_ms = now_ms;
    notify.seq = pp->current_seq;
    notify.payload.success.rtt_ms = rtt_ms;
    notify.payload.success.rssi = rssi;
    notify.payload.success.snr = snr;
    send_notify(pp, &notify);
    pp->state = PING_PONG_STATE_IDLE;
    schedule_master_auto_restart(pp, now_ms);
}

static void handle_master_fail(ping_pong_t *pp, uint32_t reason)
{
    uint32_t now_ms;
    ping_pong_notify_t notify;
    PP_TRACE(pp, "master: fail");
    now_ms = pp->port.get_time_ms();
    pp->stats.fail_count++;
    pp->stats.consecutive_fail_count++;
    pp->stats.last_fail_reason = reason;
    pp->stats.last_fail_timestamp_ms = now_ms;
    pp_memset(&notify, 0, sizeof(notify));
    notify.type = PING_PONG_NOTIFY_FAIL;
    notify.timestamp_ms = now_ms;
    notify.seq = pp->current_seq;
    notify.payload.fail.fail_reason = reason;
    send_notify(pp, &notify);
    pp->state = PING_PONG_STATE_IDLE;
    schedule_master_auto_restart(pp, now_ms);
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
    pp->state = PING_PONG_STATE_TX;
    pp->tx_start_time = pp->port.get_time_ms();
    send_tx_request(pp);
}

static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms)
{
    ping_pong_notify_t notify;
    PP_TRACE(pp, "state: enter RX_WAIT");
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
    uint8_t *buf = PP_TX_BUFFER(pp);
    if (pp->role == PING_PONG_ROLE_MASTER) {
        (void)ping_pong_build_ping_ex(buf, PING_PONG_TX_BUFFER_SIZE, pp->current_seq,
                                      pp->config.network_id,
                                      pp->config.src_id,
                                      pp->config.dst_id);
    } else {
        (void)ping_pong_build_pong_ex(buf, PING_PONG_TX_BUFFER_SIZE, pp->current_seq,
                                      pp->config.network_id,
                                      pp->config.src_id,
                                      pp->config.dst_id);
    }
    ping_pong_notify_t tx_notify;
    pp_memset(&tx_notify, 0, sizeof(tx_notify));
    tx_notify.type = PING_PONG_NOTIFY_TX_REQUEST;
    tx_notify.timestamp_ms = pp->tx_start_time;
    tx_notify.seq = pp->current_seq;
    tx_notify.payload.tx_request.tx_buffer = buf;
    tx_notify.payload.tx_request.tx_buffer_size = PING_PONG_TX_BUFFER_SIZE;
    tx_notify.payload.tx_request.tx_len = PING_PONG_PACKET_SIZE;
    pp->stats.tx_count++;
    send_notify(pp, &tx_notify);
}

void ping_pong_get_default_config(ping_pong_config_t *config)
{
    if (!config) {
        return;
    }
    config->max_retries      = PING_PONG_DEFAULT_MAX_RETRIES;
    config->rx_timeout_ms    = PING_PONG_DEFAULT_RX_TIMEOUT_MS;
    config->tx_timeout_ms    = PING_PONG_DEFAULT_TX_TIMEOUT_MS;
    config->auto_restart     = PING_PONG_DEFAULT_AUTO_RESTART;
    config->restart_delay_ms = PING_PONG_DEFAULT_RESTART_DELAY_MS;
    config->network_id       = PING_PONG_DEFAULT_NETWORK_ID;
    config->src_id           = PING_PONG_DEFAULT_SRC_ID;
    config->dst_id           = PING_PONG_DEFAULT_DST_ID;
}

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
    ping_pong_get_default_config(&pp->config);
    PP_TRACE(pp, "init: ok");
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config)
{
    if (!pp || !config) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (pp->state != PING_PONG_STATE_IDLE && pp->state != PING_PONG_STATE_STOPPED) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    pp->config = *config;
    if (!pp->config.auto_restart) {
        pp->auto_restart_pending = 0u;
        pp->next_start_time = 0u;
    }
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role)
{
    ping_pong_role_t previous_role;
    pp_event_t evt;
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    if (role == PING_PONG_ROLE_MASTER) {
        if (pp->config.rx_timeout_ms == 0u || pp->config.rx_timeout_ms > PING_PONG_MAX_TIMEOUT_MS) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
        if (pp->config.max_retries == 0u || pp->config.max_retries > PING_PONG_MAX_RETRIES) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
    }
    evt = (role == PING_PONG_ROLE_MASTER) ? EVT_START_MASTER : EVT_START_SLAVE;
    if (!is_valid_transition(pp->state, evt)) { return PING_PONG_ERR_INVALID_STATE; }
    pp->auto_restart_pending = 0u;
    pp->next_start_time = 0u;
    previous_role = pp->role;
    if (role == PING_PONG_ROLE_MASTER && previous_role == PING_PONG_ROLE_MASTER) {
        pp->current_seq++;
    }
    pp->role = role;
    pp->current_retry = 0u;
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
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_STOP)) { return PING_PONG_ERR_INVALID_STATE; }
    pp->auto_restart_pending = 0u;
    pp->next_start_time = 0u;
    pp->state = PING_PONG_STATE_STOPPED;
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_reset(ping_pong_t *pp)
{
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->current_seq = 0u;
    pp->current_retry = 0u;
    pp->tx_start_time = 0u;
    pp->rx_start_time = 0u;
    pp->auto_restart_pending = 0u;
    pp->next_start_time = 0u;
    pp_memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_process(ping_pong_t *pp)
{
    uint32_t now_ms;
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    now_ms = pp->port.get_time_ms();

    if (pp->auto_restart_pending && pp->state == PING_PONG_STATE_IDLE &&
        pp->role == PING_PONG_ROLE_MASTER && pp->config.auto_restart) {
        if (!time_reached(now_ms, pp->next_start_time)) {
            return PING_PONG_OK;
        }
        return ping_pong_start(pp, PING_PONG_ROLE_MASTER);
    }

    if (pp->state == PING_PONG_STATE_TX) {
        if (pp->config.tx_timeout_ms > 0u && (now_ms - pp->tx_start_time) >= pp->config.tx_timeout_ms) {
            pp->stats.tx_timeout_count++;
            if (pp->role == PING_PONG_ROLE_MASTER) {
                if (pp->current_retry < pp->config.max_retries) {
                    pp->current_retry++;
                    handle_master_retry(pp);
                } else {
                    handle_master_fail(pp, PING_PONG_FAIL_REASON_TX_TIMEOUT);
                }
            } else {
                PP_TRACE(pp, "slave: tx timeout, back to rx");
                enter_rx_wait(pp, now_ms);
            }
        }
        return PING_PONG_OK;
    }
    if (pp->state != PING_PONG_STATE_RX_WAIT) { return PING_PONG_OK; }
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if ((now_ms - pp->tx_start_time) >= pp->config.rx_timeout_ms) {
            if (pp->current_retry < pp->config.max_retries) {
                pp->current_retry++;
                handle_master_retry(pp);
            } else {
                pp->stats.rx_timeout_count++;
                handle_master_fail(pp, PING_PONG_FAIL_REASON_MAX_RETRIES);
            }
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (pp->config.rx_timeout_ms == 0u) { return PING_PONG_OK; }
        if ((now_ms - pp->rx_start_time) >= pp->config.rx_timeout_ms) {
            pp->stats.rx_timeout_count++;
            enter_rx_wait(pp, now_ms);
        }
    }
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp)
{
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_TX_DONE)) { return PING_PONG_ERR_INVALID_STATE; }
    enter_rx_wait(pp, pp->port.get_time_ms());
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr)
{
    uint8_t type;
    uint16_t seq;
    if (!pp || !data) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_RX_DONE)) { return PING_PONG_ERR_INVALID_STATE; }
    if (len < PING_PONG_MIN_PACKET_SIZE) { return PING_PONG_ERR_INVALID_PARAM; }
    type = packet_type(data, len);
    seq = packet_seq(data, len);
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (type == PACKET_TYPE_PONG) {
            int rc = parse_packet(pp, data, len, PACKET_TYPE_PONG, 1u);
            if (rc == PARSE_OK) {
                uint32_t rtt_ms = pp->port.get_time_ms() - pp->tx_start_time;
                pp->stats.rx_count++;
                handle_master_success(pp, rtt_ms, rssi, snr);
            } else if (rc == PARSE_ERR_CRC) {
                pp->stats.crc_error_count++;
                PP_TRACE(pp, "master: ignore crc error");
            } else {
                pp->stats.parse_error_count++;
                PP_TRACE(pp, "master: ignore parse error");
            }
        } else if (type == PACKET_TYPE_PING) {
            pp->stats.conflict_count++;
            PP_TRACE(pp, "master: ignore ping conflict");
        } else {
            pp->stats.parse_error_count++;
            PP_TRACE(pp, "master: ignore unknown packet type");
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (type == PACKET_TYPE_PING) {
            int rc = parse_packet(pp, data, len, PACKET_TYPE_PING, 0u);
            if (rc == PARSE_OK) {
                handle_slave_ping_received(pp, seq, rssi, snr);
            } else if (rc == PARSE_ERR_CRC) {
                pp->stats.crc_error_count++;
                enter_rx_wait(pp, pp->port.get_time_ms());
            } else {
                pp->stats.parse_error_count++;
                enter_rx_wait(pp, pp->port.get_time_ms());
            }
        } else if (type == PACKET_TYPE_PONG) {
            pp->stats.conflict_count++;
            enter_rx_wait(pp, pp->port.get_time_ms());
        } else {
            pp->stats.parse_error_count++;
            enter_rx_wait(pp, pp->port.get_time_ms());
        }
    }
    return PING_PONG_OK;
}

ping_pong_state_t ping_pong_get_state(const ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) { return PING_PONG_STATE_IDLE; }
    return pp->state;
}

ping_pong_role_t ping_pong_get_role(const ping_pong_t *pp)
{
    if (!pp || pp->magic != PING_PONG_MAGIC) { return PING_PONG_ROLE_NONE; }
    return pp->role;
}

ping_pong_err_t ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats)
{
    if (!pp || !stats) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
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

_Static_assert(sizeof(struct ping_pong) <= 256,
               "struct ping_pong exceeds 256-byte limit");
_Static_assert(PING_PONG_TX_BUFFER_SIZE >= PING_PONG_PACKET_SIZE,
               "PING_PONG_TX_BUFFER_SIZE must be >= PING_PONG_PACKET_SIZE");
