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

/** @brief 错误码枚举 */
typedef enum {
    PING_PONG_OK                  =  0, /**< 成功 */
    PING_PONG_ERR_NULL_PTR        = -1, /**< 空指针参数 */
    PING_PONG_ERR_NOT_INITIALIZED = -2, /**< 实例未初始化（magic 不匹配） */
    PING_PONG_ERR_INVALID_STATE   = -3, /**< 当前状态不允许此操作 */
    PING_PONG_ERR_INVALID_PARAM   = -4, /**< 参数无效 */
    PING_PONG_ERR_NOT_CONFIGURED  = -5, /**< 未设置配置 */
} ping_pong_err_t;

/** @brief 协议状态（阶段，不携带角色信息） */
typedef enum {
    PING_PONG_STATE_IDLE,        /**< 空闲 */
    PING_PONG_STATE_TX,          /**< 发送阶段 */
    PING_PONG_STATE_RX_WAIT,     /**< 等待接收阶段 */
    PING_PONG_STATE_STOPPED,     /**< 已停止 */
} ping_pong_state_t;

/** @brief 角色（与状态正交，启动时固定） */
typedef enum {
    PING_PONG_ROLE_NONE,         /**< 未确定角色 */
    PING_PONG_ROLE_MASTER,       /**< 主设备（主动发送 Ping） */
    PING_PONG_ROLE_SLAVE,        /**< 从设备（被动回复 Pong） */
} ping_pong_role_t;

/** @brief 通知类型 */
typedef enum {
    PING_PONG_NOTIFY_TX_REQUEST,    /**< 请求发送包（携带缓冲区） */
    PING_PONG_NOTIFY_RX_REQUEST,    /**< 请求进入接收模式 */
    PING_PONG_NOTIFY_SUCCESS,       /**< Ping-Pong 成功 */
    PING_PONG_NOTIFY_FAIL,          /**< Ping-Pong 失败 */
    PING_PONG_NOTIFY_RETRY,         /**< 发生重传（仅 Master） */
    PING_PONG_NOTIFY_CONFLICT,      /**< 角色冲突检测 */
    PING_PONG_NOTIFY_RX_TIMEOUT,    /**< 接收超时（仅 Slave） */
    PING_PONG_NOTIFY_PING_RECEIVED, /**< Slave 收到 Ping */
} ping_pong_notify_type_t;

/* ==================== 常量定义 ==================== */

/** @name 失败原因 */
/** @{ */
#define PING_PONG_FAIL_REASON_RX_TIMEOUT   1  /**< 接收超时 */
#define PING_PONG_FAIL_REASON_MAX_RETRIES  2  /**< 超过最大重试次数 */
#define PING_PONG_FAIL_REASON_PARSE_ERROR  3  /**< 包解析错误 */
#define PING_PONG_FAIL_REASON_CRC_ERROR    4  /**< CRC 校验失败 */
#define PING_PONG_FAIL_REASON_TX_TIMEOUT   5  /**< TX 超时 */
/** @} */

/** @name 冲突类型 */
/** @{ */
#define PING_PONG_CONFLICT_MASTER_RX_PING  1  /**< Master 收到了 Ping */
#define PING_PONG_CONFLICT_SLAVE_RX_PONG   2  /**< Slave 收到了 Pong */
/** @} */

/* ==================== 结构体定义 ==================== */

/** @brief 配置参数（必须由调用者设置） */
typedef struct {
    uint32_t timeout_ms;               /**< 等待回复的超时时间 */
    uint32_t max_retries;              /**< 最大重传次数（仅 Master） */
    uint32_t tx_buffer_size;           /**< 发送缓冲区大小 */
    uint32_t slave_rx_timeout_ms;      /**< Slave 模式下等待 Master Ping 的超时（0=永不超时） */
    uint32_t tx_timeout_ms;            /**< TX 状态超时保护（0=不检测，默认） */
} ping_pong_config_t;

/** @brief 统计信息（原始计数） */
typedef struct {
    uint32_t success_count;            /**< 成功次数 */
    uint32_t fail_count;               /**< 失败次数 */
    uint32_t retry_count;              /**< 重传次数 */
    uint32_t master_tx_count;          /**< Master 发送 Ping 次数 */
    uint32_t slave_rx_count;           /**< Slave 收到 Ping 次数 */
    uint32_t conflict_count;           /**< 冲突次数 */
    uint32_t last_rtt_ms;              /**< 最近一次 RTT */
    int16_t  last_rssi;                /**< 最近一次 RSSI */
    int16_t  last_snr;                 /**< 最近一次 SNR */
    uint32_t min_rtt_ms;               /**< 最小 RTT */
    uint32_t max_rtt_ms;               /**< 最大 RTT */
    uint32_t total_rtt_ms;             /**< 累计 RTT（用于计算平均） */
    uint32_t consecutive_success_count; /**< 连续成功次数 */
    uint32_t consecutive_fail_count;    /**< 连续失败次数 */
} ping_pong_stats_t;

/** @brief 通知数据结构 */
typedef struct ping_pong_notify {
    ping_pong_notify_type_t type;
    uint32_t timestamp_ms;
    uint32_t seq;
    union {
        struct {
            uint32_t retry_count;
        } retry;
        struct {
            uint32_t rtt_ms;
            int16_t rssi;
            int16_t snr;
        } success;
        struct {
            uint32_t fail_reason;
        } fail;
        struct {
            uint32_t conflict_type;
        } conflict;
        struct {
            uint8_t *tx_buffer;
            uint32_t tx_buffer_size;
        } tx_request;
        struct {
            uint32_t seq;
            int16_t rssi;
            int16_t snr;
        } ping_received;
    } payload;
} ping_pong_notify_t;

/** @brief 不透明结构体声明 */
typedef struct ping_pong ping_pong_t;

/** @brief 端口（PingPong 对外部的唯一依赖） */
typedef struct {
    uint32_t (*get_time_ms)(void);    /**< 获取当前时间戳（毫秒），必须实现 */
    
    /** @brief 通知回调，由上层实现
     *  @param pp      PingPong 实例指针
     *  @param notify  通知数据
     *  @param user_data 用户上下文指针 */
    void (*notify)(struct ping_pong *pp, const ping_pong_notify_t *notify,
                   void *user_data);

    void *user_data;                  /**< 用户上下文指针，传入 notify 回调 */

    /** @brief 可选 Debug Trace 钩子（NULL=不输出）
     *  @param msg 跟踪消息字符串 */
    void (*trace)(const char *msg);
} ping_pong_port_t;

/* ==================== API 函数 ==================== */

/**
 * @brief 初始化 PingPong 实例
 * @param pp   指向预分配的 PingPong 上下文（至少 sizeof(ping_pong_t) + tx_buffer_size 字节）
 * @param port 端口配置，包含必需的时间和通知回调
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_init(ping_pong_t *pp, const ping_pong_port_t *port);

/**
 * @brief 设置配置（必须在 start 前调用）
 * @param pp     PingPong 实例
 * @param config 配置参数
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_set_config(ping_pong_t *pp, const ping_pong_config_t *config);

/**
 * @brief 启动协议（角色固定）
 * @param pp   PingPong 实例
 * @param role 启动角色（MASTER 或 SLAVE）
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_start(ping_pong_t *pp, ping_pong_role_t role);

/**
 * @brief 停止协议
 * @param pp PingPong 实例
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_stop(ping_pong_t *pp);

/**
 * @brief 重置实例（清空统计和运行参数，回到 IDLE）
 * @param pp PingPong 实例
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_reset(ping_pong_t *pp);

/**
 * @brief 轮询处理（超时检测），应在主循环中周期调用
 * @param pp PingPong 实例
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_process(ping_pong_t *pp);

/**
 * @brief 发送完成通知（由 HAL 层在无线发送完成后调用）
 * @param pp PingPong 实例
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_on_tx_done(ping_pong_t *pp);

/**
 * @brief 接收完成通知（由 HAL 层在收到无线数据后调用）
 * @param pp   PingPong 实例
 * @param data 接收数据缓冲区
 * @param len  数据长度
 * @param rssi 信号强度指示
 * @param snr  信噪比
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_on_rx_done(ping_pong_t *pp, const uint8_t *data, uint32_t len,
                                      int16_t rssi, int16_t snr);

/**
 * @brief 获取当前状态
 * @param pp PingPong 实例
 * @return 当前状态枚举值
 */
ping_pong_state_t ping_pong_get_state(const ping_pong_t *pp);

/**
 * @brief 获取当前角色
 * @param pp PingPong 实例
 * @return 当前角色枚举值
 */
ping_pong_role_t ping_pong_get_role(const ping_pong_t *pp);

/**
 * @brief 获取统计信息
 * @param pp    PingPong 实例
 * @param stats 输出统计结构体
 * @return PING_PONG_OK 成功，或错误码
 */
ping_pong_err_t ping_pong_get_stats(const ping_pong_t *pp, ping_pong_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* PING_PONG_H */
