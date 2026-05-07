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

#ifndef PING_PONG_DEFAULT_MAX_RETRIES
#define PING_PONG_DEFAULT_MAX_RETRIES    3
#endif
#ifndef PING_PONG_DEFAULT_RX_TIMEOUT_MS
#define PING_PONG_DEFAULT_RX_TIMEOUT_MS  3000
#endif
#ifndef PING_PONG_DEFAULT_TX_TIMEOUT_MS
#define PING_PONG_DEFAULT_TX_TIMEOUT_MS  3000
#endif

/**
 * @brief Define a static PingPong object instance and its backing memory.
 *
 * This macro is intended for embedded/static allocation use cases. It defines
 * a byte buffer named `<name>_buffer` and a `ping_pong_t *` named `<name>`.
 * The backing size matches the current internal 256-byte context limit plus
 * the compile-time TX buffer size.
 *
 * Example:
 * @code
 * PING_PONG_DEFINE_INSTANCE(g_master);
 * ping_pong_init(g_master, &port);
 * @endcode
 */
#define PING_PONG_DEFINE_INSTANCE(name)                                      \
    static uint8_t name##_buffer[256u + PING_PONG_TX_BUFFER_SIZE];           \
    static ping_pong_t *name = (ping_pong_t *)name##_buffer

/*============================ MACROFIED FUNCTIONS ===========================*/

/* None. */

/*============================ TYPES =========================================*/

/** @brief API return codes. */
typedef enum {
    PING_PONG_OK                  =  0, /**< Operation completed successfully. */
    PING_PONG_ERR_NULL_PTR        = -1, /**< A required pointer parameter is NULL. */
    PING_PONG_ERR_NOT_INITIALIZED = -2, /**< The instance magic is invalid. */
    PING_PONG_ERR_INVALID_STATE   = -3, /**< The current state does not allow the operation. */
    PING_PONG_ERR_INVALID_PARAM   = -4, /**< A parameter value is out of range or invalid. */
} ping_pong_err_t;

/** @brief Protocol state. Role information is stored separately. */
typedef enum {
    PING_PONG_STATE_IDLE,        /**< Initialized and idle. */
    PING_PONG_STATE_TX,          /**< A packet is being transmitted. */
    PING_PONG_STATE_RX_WAIT,     /**< Waiting for a packet. */
    PING_PONG_STATE_STOPPED,     /**< Stopped explicitly by the caller. */
} ping_pong_state_t;

/** @brief Protocol role selected when the instance is started. */
typedef enum {
    PING_PONG_ROLE_NONE,         /**< No active role. */
    PING_PONG_ROLE_MASTER,       /**< Master sends Ping and waits for Pong. */
    PING_PONG_ROLE_SLAVE,        /**< Slave waits for Ping and replies with Pong. */
} ping_pong_role_t;

/** @brief Notification events emitted to the platform layer. */
typedef enum {
    PING_PONG_NOTIFY_TX_REQUEST, /**< Start radio TX with the provided buffer and tx_len. */
    PING_PONG_NOTIFY_RX_REQUEST, /**< Start radio RX mode. */
    PING_PONG_NOTIFY_SUCCESS,    /**< Master completed one Ping-Pong round. */
    PING_PONG_NOTIFY_FAIL,       /**< Master failed the current Ping-Pong round. */
    PING_PONG_NOTIFY_RX_TIMEOUT, /**< Slave RX timeout; the core re-enters RX wait. */
} ping_pong_notify_type_t;

/** @brief Runtime configuration. Defaults are populated by ping_pong_init(). */
typedef struct {
    uint32_t max_retries;   /**< Maximum retry count for MASTER. Ignored by SLAVE. */
    uint32_t rx_timeout_ms; /**< RX timeout. MASTER requires >0; SLAVE uses 0 for endless listening. */
    uint32_t tx_timeout_ms; /**< TX timeout protection. Set 0 to disable. */
} ping_pong_config_t;

/** @brief Raw protocol statistics. */
typedef struct {
    uint32_t success_count;     /**< Successful rounds, MASTER only. */
    uint32_t fail_count;        /**< Failed rounds, MASTER only. */
    uint32_t retry_count;       /**< Retry count, MASTER only. */
    uint32_t tx_count;          /**< Transmitted packet count. */
    uint32_t rx_count;          /**< Valid received packet count. */
    uint32_t conflict_count;    /**< Role conflict count. */
    uint32_t crc_error_count;   /**< CRC validation error count. */
    uint32_t parse_error_count; /**< Packet format, type, or sequence error count. */
    uint32_t rx_timeout_count;  /**< RX timeout count. */
    uint32_t tx_timeout_count;  /**< TX timeout count. */
    uint32_t last_rtt_ms;       /**< Last measured RTT, MASTER only. */
    int16_t  last_rssi;         /**< Last received RSSI. */
    int16_t  last_snr;          /**< Last received SNR. */
} ping_pong_stats_t;

/** @brief Notification payload passed to ping_pong_port_t::notify. */
typedef struct ping_pong_notify {
    ping_pong_notify_type_t type; /**< Notification type. */
    uint32_t timestamp_ms;        /**< Timestamp from get_time_ms(). */
    uint32_t seq;                 /**< Current packet sequence. */
    union {
        struct {
            uint32_t rtt_ms;      /**< Round-trip time in milliseconds. */
            int16_t rssi;         /**< RSSI of the received Pong. */
            int16_t snr;          /**< SNR of the received Pong. */
        } success;
        struct {
            uint32_t fail_reason; /**< One of PING_PONG_FAIL_REASON_*. */
        } fail;
        struct {
            uint8_t *tx_buffer;      /**< Core-built packet buffer. */
            uint32_t tx_buffer_size; /**< Buffer capacity only; do not use as TX length. */
            uint32_t tx_len;         /**< Exact packet length to send. */
        } tx_request;
    } payload;
} ping_pong_notify_t;

typedef struct ping_pong ping_pong_t;

/** @brief Platform port callbacks and user context. */
typedef struct {
    uint32_t (*get_time_ms)(void); /**< Required millisecond timestamp provider. */
    void (*notify)(struct ping_pong *pp, const ping_pong_notify_t *notify,
                   void *user_data); /**< Required notification callback. */
    void *user_data;                 /**< User context passed back to notify(). */
    void (*trace)(const char *msg);  /**< Optional debug trace hook. */
} ping_pong_port_t;

/*============================ GLOBAL VARIABLES ==============================*/

/* None. */

/*============================ LOCAL VARIABLES ===============================*/

/* None. */

/*============================ PROTOTYPES ====================================*/

/**
 * @brief Return the number of bytes required for a PingPong instance.
 *
 * Allocate at least this many bytes and cast the buffer to ping_pong_t * before
 * calling ping_pong_init(). The size includes the opaque context and internal TX buffer.
 */
uint32_t ping_pong_instance_size(void);

/**
 * @brief Initialize a PingPong instance and load default configuration values.
 * @param pp Preallocated instance memory.
 * @param port Platform callbacks; get_time_ms and notify are required.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port);

/**
 * @brief Override runtime configuration while the instance is idle or stopped.
 * @param pp PingPong instance.
 * @param config New configuration values.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config);

/**
 * @brief Start the protocol as MASTER or SLAVE.
 * @param pp PingPong instance.
 * @param role PING_PONG_ROLE_MASTER or PING_PONG_ROLE_SLAVE.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role);

/**
 * @brief Stop a running instance.
 * @param pp PingPong instance.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_stop(ping_pong_t *pp);

/**
 * @brief Reset runtime state and statistics back to IDLE.
 * @param pp PingPong instance.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_reset(ping_pong_t *pp);

/**
 * @brief Drive timeout handling. Call periodically from the main loop.
 * @param pp PingPong instance.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_process(ping_pong_t *pp);

/**
 * @brief Notify the core that the platform radio TX operation has completed.
 * @param pp PingPong instance.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp);

/**
 * @brief Notify the core that a platform radio RX operation has completed.
 * @param pp PingPong instance.
 * @param data Received bytes.
 * @param len Received byte count.
 * @param rssi Received signal strength.
 * @param snr Signal-to-noise ratio.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr);

/** @brief Return the current protocol state, or IDLE for an invalid instance. */
ping_pong_state_t ping_pong_get_state(const ping_pong_t *pp);

/** @brief Return the current role, or NONE for an invalid instance. */
ping_pong_role_t ping_pong_get_role(const ping_pong_t *pp);

/**
 * @brief Copy current statistics.
 * @param pp PingPong instance.
 * @param stats Output statistics structure.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats);

/** @brief Return 1 if the instance pointer looks initialized, otherwise 0. */
int ping_pong_is_valid(const ping_pong_t *pp);

/**
 * @brief Build a fixed-size Ping packet.
 * @param buf Output buffer.
 * @param buf_size Output buffer capacity. Must be at least PING_PONG_PACKET_SIZE.
 * @param seq 16-bit sequence number.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_build_ping(uint8_t *buf, uint32_t buf_size, uint16_t seq);

/**
 * @brief Build a fixed-size Pong packet.
 * @param buf Output buffer.
 * @param buf_size Output buffer capacity. Must be at least PING_PONG_PACKET_SIZE.
 * @param seq 16-bit sequence number.
 * @return PING_PONG_OK or an error code.
 */
ping_pong_err_t ping_pong_build_pong(uint8_t *buf, uint32_t buf_size, uint16_t seq);

/*============================ IMPLEMENTATION ================================*/

/* None. */

#ifdef __cplusplus
}
#endif

#endif /* PING_PONG_H */
