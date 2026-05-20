/*
 * PingPong 中间件实现 - 主从一体版
 */

#include "ping_pong.h"

#define PING_PONG_MAGIC 0x50494E47
#define PACKET_TYPE_PING  0x01
#define PACKET_TYPE_PONG  0x02
#define PING_PONG_CRC_SIZE 2
#define PP_MIN_PACKET_SIZE (sizeof(ping_pong_header_t) + PING_PONG_CRC_SIZE)
#define PARSE_OK          0
#define PARSE_ERR_FORMAT (-1)
#define PARSE_ERR_CRC    (-2)

#define PP_TX_BUFFER(pp) ((uint8_t *)(pp) + sizeof(struct ping_pong))

#define PP_TRACE(pp, msg) do { \
    if ((pp)->port.trace) { (pp)->port.trace(msg); } \
} while (0)

typedef struct {
    uint8_t type;
    uint8_t seq_hi;
    uint8_t seq_lo;
    uint8_t reserved;
} ping_pong_header_t;

typedef enum {
    EVT_START_MASTER,
    EVT_START_SLAVE,
    EVT_STOP,
    EVT_TX_DONE,
    EVT_RX_DONE,
    EVT_TICK,
    EVT_COUNT
} pp_event_t;

typedef struct {
    uint32_t now_ms;
    const uint8_t *rx_data;
    uint32_t rx_len;
    int16_t rssi;
    int16_t snr;
} pp_event_data_t;

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
static ping_pong_err_t pp_dispatch(ping_pong_t *pp, pp_event_t event,
                                   const pp_event_data_t *data);

static const uint8_t valid_transitions[4][EVT_COUNT] = {
    [PING_PONG_STATE_IDLE] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
        [EVT_TICK] = 1,
    },
    [PING_PONG_STATE_TX] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 1, [EVT_RX_DONE] = 0,
        [EVT_TICK] = 1,
    },
    [PING_PONG_STATE_RX_WAIT] = {
        [EVT_START_MASTER] = 0, [EVT_START_SLAVE] = 0,
        [EVT_STOP] = 1, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 1,
        [EVT_TICK] = 1,
    },
    [PING_PONG_STATE_STOPPED] = {
        [EVT_START_MASTER] = 1, [EVT_START_SLAVE] = 1,
        [EVT_STOP] = 0, [EVT_TX_DONE] = 0, [EVT_RX_DONE] = 0,
        [EVT_TICK] = 1,
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

static int is_valid_transition(ping_pong_state_t state, pp_event_t event)
{
    if ((unsigned)state >= 4 || (unsigned)event >= EVT_COUNT) {
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

static ping_pong_err_t build_packet(uint8_t *buf, uint32_t buf_size, uint8_t type, uint16_t seq)
{
    uint16_t crc;
    if (!buf) {
        return PING_PONG_ERR_NULL_PTR;
    }
    if (buf_size < PING_PONG_PACKET_SIZE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    buf[0] = type;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    crc = compute_crc16(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_build_ping(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    return build_packet(buf, buf_size, PACKET_TYPE_PING, seq);
}

ping_pong_err_t ping_pong_build_pong(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    return build_packet(buf, buf_size, PACKET_TYPE_PONG, seq);
}

static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type)
{
    if (!data || len < PP_MIN_PACKET_SIZE) {
        return PARSE_ERR_FORMAT;
    }
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
    uint16_t pkt_seq = get_u16_be(&header->seq_hi);
    if (pkt_seq != pp->current_seq) {
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
    PP_TRACE(pp, "master: success");
    now_ms = pp->port.get_time_ms();
    pp->stats.success_count++;
    pp->stats.consecutive_fail_count = 0;
    pp->stats.last_rtt_ms = rtt_ms;
    pp->stats.last_rssi = rssi;
    pp->stats.last_snr = snr;
    ping_pong_notify_t notify;
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
    PP_TRACE(pp, "master: fail");
    now_ms = pp->port.get_time_ms();
    pp->stats.fail_count++;
    pp->stats.consecutive_fail_count++;
    pp->stats.last_fail_reason = reason;
    pp->stats.last_fail_timestamp_ms = now_ms;
    ping_pong_notify_t notify;
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
    uint8_t *buf = PP_TX_BUFFER(pp);
    if (pp->role == PING_PONG_ROLE_MASTER) {
        (void)ping_pong_build_ping(buf, PING_PONG_TX_BUFFER_SIZE, pp->current_seq);
    } else {
        (void)ping_pong_build_pong(buf, PING_PONG_TX_BUFFER_SIZE, pp->current_seq);
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

static ping_pong_err_t pp_dispatch(ping_pong_t *pp, pp_event_t event,
                                   const pp_event_data_t *data)
{
    uint32_t now_ms;
    ping_pong_role_t previous_role;

    if (!is_valid_transition(pp->state, event)) {
        return PING_PONG_ERR_INVALID_STATE;
    }

    now_ms = data ? data->now_ms : pp->port.get_time_ms();

    switch (event) {
    case EVT_START_MASTER:
        pp->auto_restart_pending = 0u;
        pp->next_start_time = 0u;
        previous_role = pp->role;
        if (previous_role == PING_PONG_ROLE_MASTER) {
            pp->current_seq++;
        }
        pp->role = PING_PONG_ROLE_MASTER;
        pp->current_retry = 0;
        pp->state = PING_PONG_STATE_TX;
        pp->tx_start_time = now_ms;
        send_tx_request(pp);
        return PING_PONG_OK;

    case EVT_START_SLAVE:
        pp->auto_restart_pending = 0u;
        pp->next_start_time = 0u;
        pp->role = PING_PONG_ROLE_SLAVE;
        pp->current_retry = 0;
        enter_rx_wait(pp, now_ms);
        return PING_PONG_OK;

    case EVT_STOP:
        pp->auto_restart_pending = 0u;
        pp->next_start_time = 0u;
        pp->state = PING_PONG_STATE_STOPPED;
        return PING_PONG_OK;

    case EVT_TX_DONE:
        enter_rx_wait(pp, now_ms);
        return PING_PONG_OK;

    case EVT_TICK:
        if (pp->auto_restart_pending && pp->state == PING_PONG_STATE_IDLE &&
            pp->role == PING_PONG_ROLE_MASTER && pp->config.auto_restart) {
            if (!time_reached(now_ms, pp->next_start_time)) {
                return PING_PONG_OK;
            }
            return pp_dispatch(pp, EVT_START_MASTER, data);
        }

        if (pp->state == PING_PONG_STATE_TX) {
            if (pp->config.tx_timeout_ms > 0 &&
                (now_ms - pp->tx_start_time) >= pp->config.tx_timeout_ms) {
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

        if (pp->state != PING_PONG_STATE_RX_WAIT) {
            return PING_PONG_OK;
        }

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
            if (pp->config.rx_timeout_ms == 0) {
                return PING_PONG_OK;
            }
            if ((now_ms - pp->rx_start_time) >= pp->config.rx_timeout_ms) {
                pp->stats.rx_timeout_count++;
                enter_rx_wait(pp, now_ms);
            }
        }
        return PING_PONG_OK;

    case EVT_RX_DONE:
    default:
        return PING_PONG_ERR_INVALID_STATE;
    }
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
    pp_event_t evt;
    pp_event_data_t data;

    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    if (role == PING_PONG_ROLE_MASTER) {
        if (pp->config.rx_timeout_ms == 0 || pp->config.rx_timeout_ms > PING_PONG_MAX_TIMEOUT_MS) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
        if (pp->config.max_retries == 0 || pp->config.max_retries > PING_PONG_MAX_RETRIES) {
            return PING_PONG_ERR_INVALID_PARAM;
        }
    }

    evt = (role == PING_PONG_ROLE_MASTER) ? EVT_START_MASTER : EVT_START_SLAVE;
    pp_memset(&data, 0, sizeof(data));
    data.now_ms = pp->port.get_time_ms();
    return pp_dispatch(pp, evt, &data);
}

ping_pong_err_t ping_pong_stop(ping_pong_t *pp)
{
    pp_event_data_t data;

    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }

    pp_memset(&data, 0, sizeof(data));
    data.now_ms = pp->port.get_time_ms();
    return pp_dispatch(pp, EVT_STOP, &data);
}

ping_pong_err_t ping_pong_reset(ping_pong_t *pp)
{
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->current_seq = 0;
    pp->current_retry = 0;
    pp->tx_start_time = 0;
    pp->rx_start_time = 0;
    pp->auto_restart_pending = 0u;
    pp->next_start_time = 0u;
    pp_memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_process(ping_pong_t *pp)
{
    pp_event_data_t data;

    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }

    pp_memset(&data, 0, sizeof(data));
    data.now_ms = pp->port.get_time_ms();
    return pp_dispatch(pp, EVT_TICK, &data);
}

ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp)
{
    pp_event_data_t data;

    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }

    pp_memset(&data, 0, sizeof(data));
    data.now_ms = pp->port.get_time_ms();
    return pp_dispatch(pp, EVT_TX_DONE, &data);
}

ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr)
{
    if (!pp || !data) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_RX_DONE)) { return PING_PONG_ERR_INVALID_STATE; }
    if (len < PP_MIN_PACKET_SIZE) { return PING_PONG_ERR_INVALID_PARAM; }
    const ping_pong_header_t *header = (const ping_pong_header_t *)data;
    uint16_t seq = get_u16_be(&header->seq_hi);
    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (header->type == PACKET_TYPE_PONG) {
            int rc = parse_packet(pp, data, len, PACKET_TYPE_PONG);
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
        } else if (header->type == PACKET_TYPE_PING) {
            pp->stats.conflict_count++;
            PP_TRACE(pp, "master: ignore ping conflict");
        } else {
            pp->stats.parse_error_count++;
            PP_TRACE(pp, "master: ignore unknown packet type");
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (header->type == PACKET_TYPE_PING) {
            uint32_t header_len = len - PING_PONG_CRC_SIZE;
            uint16_t computed = compute_crc16(data, header_len);
            uint16_t received = get_u16_be(&data[header_len]);
            if (computed != received) {
                pp->stats.crc_error_count++;
                enter_rx_wait(pp, pp->port.get_time_ms());
            } else {
                handle_slave_ping_received(pp, seq, rssi, snr);
            }
        } else if (header->type == PACKET_TYPE_PONG) {
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
_Static_assert(PING_PONG_TX_BUFFER_SIZE >= PING_PONG_MIN_PACKET_SIZE,
               "PING_PONG_TX_BUFFER_SIZE must be >= PING_PONG_MIN_PACKET_SIZE");
