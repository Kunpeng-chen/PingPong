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

/*============================ INCLUDES ======================================*/

#include <stdint.h>
#include <stddef.h>
#include "ping_pong_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================ MACROS ========================================*/

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

/**
 * @brief Define a static PingPong object instance and its backing memory.
 *
 * This macro is intended for embedded/static allocation use cases. It defines
 * a byte buffer named `<name>_buffer` and a `ping_pong_t *` named `<name>`.
 * The backing size matches the current internal 256-byte context limit plus
 * the compile-time TX buffer size.
 */
#define PING_PONG_DEFINE_INSTANCE(name)                                      \
    static uint8_t name##_buffer[256u + PING_PONG_TX_BUFFER_SIZE];           \
    static ping_pong_t *name = (ping_pong_t *)name##_buffer

/*============================ TYPES =========================================*/

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
} ping_pong_notify_type_t;

/** @brief Runtime configuration. Defaults are populated by ping_pong_init(). */
typedef struct {
    uint32_t max_retries;      /**< Maximum retry count for MASTER. Ignored by SLAVE. */
    uint32_t rx_timeout_ms;    /**< RX timeout. MASTER requires >0; SLAVE uses 0 for endless listening. */
    uint32_t tx_timeout_ms;    /**< TX timeout protection. Set 0 to disable. */
    uint8_t  auto_restart;     /**< MASTER automatically starts next round after SUCCESS/FAIL when non-zero. */
    uint32_t restart_delay_ms; /**< Delay before auto-restarting the next MASTER round. */
} ping_pong_config_t;

/** @brief Raw protocol statistics. */
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
    uint32_t consecutive_fail_count;  /**< Consecutive MASTER round failures since the last success. */
    uint32_t last_fail_reason;        /**< Last PING_PONG_FAIL_REASON_* value. */
    uint32_t last_fail_timestamp_ms;  /**< Timestamp of the last MASTER failure. */
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
            uint32_t tx_buffer_size;
            uint32_t tx_len;
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
void ping_pong_get_default_config(ping_pong_config_t *config);
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
