/*
 * PingPong 中间件实现 - 主从一体版
 */

#include "ping_pong.h"

#define PING_PONG_MAGIC 0x50494E47u
#define PACKET_MAGIC_HI 0x50u
#define PACKET_MAGIC_LO 0x50u
#define PACKET_TYPE_PING  0x01u
#define PACKET_TYPE_PONG  0x02u
#define PING_PONG_CRC_SIZE 2u
#define PP_PACKET_CRC_OFFSET 22u
#define PARSE_OK               0
#define PARSE_ERR_FORMAT      -1
#define PARSE_ERR_CRC         -2
#define PARSE_ERR_NETWORK     -3
#define PARSE_ERR_ADDRESS     -4
#define PARSE_ERR_SEQ         -5
#define PARSE_ERR_PEER        -6

#define PP_TX_BUFFER(pp) ((uint8_t *)(pp) + sizeof(struct ping_pong))
#define PP_TRACE(pp, msg) do { if ((pp)->port.trace) { (pp)->port.trace(msg); } } while (0)

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint8_t src_id;
    uint8_t dst_id;
} parsed_packet_t;

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
    ping_pong_identity_t identity;
    ping_pong_port_t port;
    uint16_t current_seq;
    uint16_t current_retry;
    uint32_t tx_start_time;
    uint32_t rx_start_time;
    ping_pong_stats_t stats;
};

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
    for (i = 0; i < len; i++) { p[i] = (uint8_t)val; }
}

static inline uint16_t get_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static int is_valid_transition(ping_pong_state_t state, pp_event_t event)
{
    if ((unsigned)state >= 4u || (unsigned)event >= EVT_COUNT) { return 0; }
    return valid_transitions[state][event];
}

static void send_notify(ping_pong_t *pp, const ping_pong_notify_t *notify)
{
    if (pp->port.notify) { pp->port.notify(pp, notify, pp->port.user_data); }
}

static uint16_t compute_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t j;
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; j++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static ping_pong_identity_t default_master_identity(void)
{
    ping_pong_identity_t id = {
        .local_id = PING_PONG_DEFAULT_LOCAL_ID,
        .peer_id = PING_PONG_DEFAULT_PEER_ID,
        .network_id = PING_PONG_DEFAULT_NETWORK_ID,
        .allow_broadcast = 0u,
    };
    return id;
}

static ping_pong_err_t build_packet_with_identity(uint8_t *buf, uint32_t buf_size,
                                                  uint8_t type, uint16_t seq,
                                                  const ping_pong_identity_t *identity,
                                                  uint8_t reverse)
{
    uint16_t crc;
    uint8_t src;
    uint8_t dst;
    if (!buf || !identity) { return PING_PONG_ERR_NULL_PTR; }
    if (buf_size < PING_PONG_PACKET_SIZE) { return PING_PONG_ERR_INVALID_PARAM; }

    src = reverse ? identity->peer_id : identity->local_id;
    dst = reverse ? identity->local_id : identity->peer_id;

    pp_memset(buf, 0, PING_PONG_PACKET_SIZE);
    buf[0] = PACKET_MAGIC_HI;
    buf[1] = PACKET_MAGIC_LO;
    buf[2] = PING_PONG_PROTOCOL_VERSION;
    buf[3] = type;
    buf[4] = 0u;
    buf[5] = (uint8_t)(seq >> 8);
    buf[6] = (uint8_t)(seq & 0xFFu);
    buf[7] = identity->network_id;
    buf[8] = src;
    buf[9] = dst;

    crc = compute_crc16(buf, PP_PACKET_CRC_OFFSET);
    buf[22] = (uint8_t)(crc >> 8);
    buf[23] = (uint8_t)(crc & 0xFFu);
    return PING_PONG_OK;
}

static ping_pong_err_t build_packet(uint8_t *buf, uint32_t buf_size, uint8_t type, uint16_t seq)
{
    ping_pong_identity_t id = default_master_identity();
    return build_packet_with_identity(buf, buf_size, type, seq, &id, 0u);
}

ping_pong_err_t ping_pong_build_ping(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    return build_packet(buf, buf_size, PACKET_TYPE_PING, seq);
}

ping_pong_err_t ping_pong_build_pong(uint8_t *buf, uint32_t buf_size, uint16_t seq)
{
    ping_pong_identity_t id = default_master_identity();
    return build_packet_with_identity(buf, buf_size, PACKET_TYPE_PONG, seq, &id, 1u);
}

static int parse_packet(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                        uint8_t expected_type, parsed_packet_t *packet)
{
    uint16_t computed_crc;
    uint16_t received_crc;
    uint16_t pkt_seq;
    uint8_t dst_id;
    if (!data || !packet || len < PING_PONG_PACKET_SIZE) { return PARSE_ERR_FORMAT; }
    if (data[0] != PACKET_MAGIC_HI || data[1] != PACKET_MAGIC_LO ||
        data[2] != PING_PONG_PROTOCOL_VERSION) {
        return PARSE_ERR_FORMAT;
    }
    computed_crc = compute_crc16(data, PP_PACKET_CRC_OFFSET);
    received_crc = get_u16_be(&data[PP_PACKET_CRC_OFFSET]);
    if (computed_crc != received_crc) { return PARSE_ERR_CRC; }
    if (data[7] != pp->identity.network_id) { return PARSE_ERR_NETWORK; }

    dst_id = data[9];
    if (dst_id != pp->identity.local_id) {
        if (!(pp->identity.allow_broadcast && dst_id == PING_PONG_BROADCAST_ID)) {
            return PARSE_ERR_ADDRESS;
        }
    }
    if (data[8] != pp->identity.peer_id) { return PARSE_ERR_PEER; }
    if (data[3] != expected_type) { return PARSE_ERR_FORMAT; }

    pkt_seq = get_u16_be(&data[5]);
    if (pkt_seq != pp->current_seq) { return PARSE_ERR_SEQ; }

    packet->type = data[3];
    packet->seq = pkt_seq;
    packet->src_id = data[8];
    packet->dst_id = data[9];
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

static void enter_rx_wait(ping_pong_t *pp, uint32_t timestamp_ms);
static void send_tx_request(ping_pong_t *pp);

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
        (void)build_packet_with_identity(buf, PING_PONG_TX_BUFFER_SIZE, PACKET_TYPE_PING,
                                         pp->current_seq, &pp->identity, 0u);
    } else {
        (void)build_packet_with_identity(buf, PING_PONG_TX_BUFFER_SIZE, PACKET_TYPE_PONG,
                                         pp->current_seq, &pp->identity, 0u);
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

ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port)
{
    if (!pp || !port || !port->get_time_ms || !port->notify) { return PING_PONG_ERR_NULL_PTR; }
    pp_memset(pp, 0, sizeof(ping_pong_t));
    pp->magic = PING_PONG_MAGIC;
    pp->state = PING_PONG_STATE_IDLE;
    pp->role = PING_PONG_ROLE_NONE;
    pp->port = *port;
    pp->config.max_retries   = PING_PONG_DEFAULT_MAX_RETRIES;
    pp->config.rx_timeout_ms = PING_PONG_DEFAULT_RX_TIMEOUT_MS;
    pp->config.tx_timeout_ms = PING_PONG_DEFAULT_TX_TIMEOUT_MS;
    pp->identity = default_master_identity();
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
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_set_identity(ping_pong_t *pp, const ping_pong_identity_t *identity)
{
    if (!pp || !identity) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (pp->state != PING_PONG_STATE_IDLE && pp->state != PING_PONG_STATE_STOPPED) {
        return PING_PONG_ERR_INVALID_STATE;
    }
    if (identity->local_id == PING_PONG_BROADCAST_ID || identity->peer_id == PING_PONG_BROADCAST_ID) {
        return PING_PONG_ERR_INVALID_PARAM;
    }
    pp->identity = *identity;
    pp->identity.allow_broadcast = identity->allow_broadcast ? 1u : 0u;
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role)
{
    ping_pong_role_t previous_role;
    pp_event_t evt;
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (role != PING_PONG_ROLE_MASTER && role != PING_PONG_ROLE_SLAVE) { return PING_PONG_ERR_INVALID_PARAM; }
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
    previous_role = pp->role;
    if (role == PING_PONG_ROLE_MASTER && previous_role == PING_PONG_ROLE_MASTER) { pp->current_seq++; }
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
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_STOP)) { return PING_PONG_ERR_INVALID_STATE; }
    pp->state = PING_PONG_STATE_STOPPED;
    return PING_PONG_OK;
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
    pp_memset(&pp->stats, 0, sizeof(ping_pong_stats_t));
    return PING_PONG_OK;
}

ping_pong_err_t ping_pong_process(ping_pong_t *pp)
{
    uint32_t now_ms;
    if (!pp) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    now_ms = pp->port.get_time_ms();
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

static void record_parse_failure(ping_pong_t *pp, int rc)
{
    if (rc == PARSE_ERR_CRC) {
        pp->stats.crc_error_count++;
    } else if (rc == PARSE_ERR_NETWORK) {
        pp->stats.network_filter_count++;
    } else if (rc == PARSE_ERR_ADDRESS) {
        pp->stats.address_filter_count++;
    } else {
        pp->stats.parse_error_count++;
    }
}

ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr)
{
    parsed_packet_t pkt;
    int rc;
    uint8_t rx_type;
    if (!pp || !data) { return PING_PONG_ERR_NULL_PTR; }
    if (pp->magic != PING_PONG_MAGIC) { return PING_PONG_ERR_NOT_INITIALIZED; }
    if (!is_valid_transition(pp->state, EVT_RX_DONE)) { return PING_PONG_ERR_INVALID_STATE; }
    if (len < PING_PONG_PACKET_SIZE) { return PING_PONG_ERR_INVALID_PARAM; }
    rx_type = data[3];

    if (pp->role == PING_PONG_ROLE_MASTER) {
        if (rx_type == PACKET_TYPE_PONG) {
            rc = parse_packet(pp, data, len, PACKET_TYPE_PONG, &pkt);
            if (rc == PARSE_OK) {
                uint32_t rtt_ms = pp->port.get_time_ms() - pp->tx_start_time;
                pp->stats.rx_count++;
                handle_master_success(pp, rtt_ms, rssi, snr);
            } else {
                record_parse_failure(pp, rc);
                PP_TRACE(pp, "master: ignore invalid pong");
            }
        } else if (rx_type == PACKET_TYPE_PING) {
            pp->stats.conflict_count++;
            PP_TRACE(pp, "master: ignore ping conflict");
        } else {
            pp->stats.parse_error_count++;
            PP_TRACE(pp, "master: ignore unknown packet type");
        }
    } else if (pp->role == PING_PONG_ROLE_SLAVE) {
        if (rx_type == PACKET_TYPE_PING) {
            rc = parse_packet(pp, data, len, PACKET_TYPE_PING, &pkt);
            if (rc == PARSE_OK) {
                handle_slave_ping_received(pp, pkt.seq, rssi, snr);
            } else {
                record_parse_failure(pp, rc);
                enter_rx_wait(pp, pp->port.get_time_ms());
            }
        } else if (rx_type == PACKET_TYPE_PONG) {
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
