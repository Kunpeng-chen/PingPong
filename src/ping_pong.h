/**
 * @file ping_pong.h
 * @brief PingPong 中间件 - 主从一体版
 *
 * 设计原则：
 * - 只做协议：状态机 + 编解码 + 超时重传 + 主从角色管理
 * - 零依赖：不依赖任何外部模块
 * - 时间由端口注入，事件通过回调通知
 * - 状态与角色分离，角色启动时固定
 */

#ifndef PING_PONG_H
#define PING_PONG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 枚举定义 ==================== */

typedef enum {
    PING_PONG_OK                  =  0,
    PING_PONG_ERR_NULL_PTR        = -1,
    PING_PONG_ERR_NOT_INITIALIZED = -2,
    PING_PONG_ERR_INVALID_STATE   = -3,
    PING_PONG_ERR_INVALID_PARAM   = -4,
} ping_pong_err_t;

typedef enum {
    PING_PONG_STATE_IDLE,
    PING_PONG_STATE_TX,
    PING_PONG_STATE_RX_WAIT,
    PING_PONG_STATE_STOPPED,
} ping_pong_state_t;

typedef enum {
    PING_PONG_ROLE_NONE,
    PING_PONG_ROLE_MASTER,
    PING_PONG_ROLE_SLAVE,
} ping_pong_role_t;

typedef enum {
    PING_PONG_NOTIFY_TX_REQUEST,
    PING_PONG_NOTIFY_RX_REQUEST,
    PING_PONG_NOTIFY_SUCCESS,
    PING_PONG_NOTIFY_FAIL,
    PING_PONG_NOTIFY_RX_TIMEOUT,
} ping_pong_notify_type_t;

#define PING_PONG_FAIL_REASON_RX_TIMEOUT   1
#define PING_PONG_FAIL_REASON_MAX_RETRIES  2
#define PING_PONG_FAIL_REASON_PARSE_ERROR  3
#define PING_PONG_FAIL_REASON_CRC_ERROR    4
#define PING_PONG_FAIL_REASON_TX_TIMEOUT   5
#define PING_PONG_FAIL_REASON_CONFLICT     6

#define PING_PONG_MIN_PACKET_SIZE  6
#define PING_PONG_PACKET_SIZE      PING_PONG_MIN_PACKET_SIZE

#ifndef PING_PONG_MAX_TIMEOUT_MS
#define PING_PONG_MAX_TIMEOUT_MS   600000
#endif
#ifndef PING_PONG_MAX_RETRIES
#define PING_PONG_MAX_RETRIES      255
#endif

#ifndef PING_PONG_TX_BUFFER_SIZE
#define PING_PONG_TX_BUFFER_SIZE   PING_PONG_MIN_PACKET_SIZE
#endif

#ifndef PING_PONG_DEFAULT_MAX_RETRIES
#define PING_PONG_DEFAULT_MAX_RETRIES    3
#endif
#ifndef PING_PONG_DEFAULT_RX_TIMEOUT_MS
#define PING_PONG_DEFAULT_RX_TIMEOUT_MS  3000
#endif
#ifndef PING_PONG_DEFAULT_TX_TIMEOUT_MS
#define PING_PONG_DEFAULT_TX_TIMEOUT_MS  3000
#endif

typedef struct {
    uint32_t max_retries;
    uint32_t rx_timeout_ms;
    uint32_t tx_timeout_ms;
} ping_pong_config_t;

typedef struct {
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t retry_count;
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t conflict_count;
    uint32_t crc_error_count;
    uint32_t parse_error_count;
    uint32_t rx_timeout_count;
    uint32_t tx_timeout_count;
    uint32_t last_rtt_ms;
    int16_t  last_rssi;
    int16_t  last_snr;
} ping_pong_stats_t;

typedef struct ping_pong_notify {
    ping_pong_notify_type_t type;
    uint32_t timestamp_ms;
    uint32_t seq;
    union {
        struct {
            uint32_t rtt_ms;
            int16_t rssi;
            int16_t snr;
        } success;
        struct {
            uint32_t fail_reason;
        } fail;
        struct {
            uint8_t *tx_buffer;
            uint32_t tx_buffer_size; /* capacity, kept for compatibility */
            uint32_t tx_len;         /* exact number of bytes to send */
        } tx_request;
    } payload;
} ping_pong_notify_t;

typedef struct ping_pong ping_pong_t;

typedef struct {
    uint32_t (*get_time_ms)(void);
    void (*notify)(struct ping_pong *pp, const ping_pong_notify_t *notify,
                   void *user_data);
    void *user_data;
    void (*trace)(const char *msg);
} ping_pong_port_t;

uint32_t ping_pong_instance_size(void);
ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port);
ping_pong_err_t ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config);
ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role);
ping_pong_err_t ping_pong_stop(ping_pong_t *pp);
ping_pong_err_t ping_pong_reset(ping_pong_t *pp);
ping_pong_err_t ping_pong_process(ping_pong_t *pp);
ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp);
ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr);
ping_pong_state_t ping_pong_get_state(const ping_pong_t *pp);
ping_pong_role_t ping_pong_get_role(const ping_pong_t *pp);
ping_pong_err_t ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats);
int ping_pong_is_valid(const ping_pong_t *pp);

ping_pong_err_t ping_pong_build_ping(uint8_t *buf, uint32_t buf_size, uint16_t seq);
ping_pong_err_t ping_pong_build_pong(uint8_t *buf, uint32_t buf_size, uint16_t seq);

#ifdef __cplusplus
}
#endif

#endif /* PING_PONG_H */
