/*
 * PingPong 中间件使用示例
 * 硬件平台: 假设有 Radio 和 Timer 外设
 * 功能: Master 持续发送 Ping，Slave 响应 Pong
 */

#include "ping_pong.h"

/* ==================== 硬件抽象层接口（需用户实现） ==================== */
extern uint32_t hal_get_tick_ms(void);           /* 毫秒时间戳 */
extern void hal_radio_send(const uint8_t *buf, uint32_t len);  /* 非阻塞发送 */
extern void hal_radio_start_rx(void);            /* 启动接收 */
extern bool hal_radio_tx_done(void);             /* 发送完成标志 */
extern bool hal_radio_rx_done(void);             /* 接收完成标志 */
extern void hal_radio_clear_tx_done(void);
extern void hal_radio_clear_rx_done(void);
extern const uint8_t* hal_radio_get_rx_data(uint32_t *len);
extern int16_t hal_radio_get_rssi(void);
extern int16_t hal_radio_get_snr(void);

/* ==================== 应用上下文 ==================== */
typedef struct {
    ping_pong_t pp;          /* PingPong 实例 */
    uint8_t tx_buf[64];      /* 发送缓冲区（大小与配置一致） */
    uint8_t rx_buf[64];      /* 接收缓冲区（可选，用于临时存储） */
} app_context_t;

static app_context_t app;

/* ==================== 重入保护标志 ==================== */
static volatile struct {
    uint8_t restart_master;
    uint8_t restart_slave;
} pending_flags;

/* ==================== 包构建函数 ==================== */
static void build_ping_packet(uint8_t *buf, uint32_t seq)
{
    buf[0] = 0x01;  /* type = PING */
    buf[1] = (uint8_t)(seq >> 8);    /* seq_hi */
    buf[2] = (uint8_t)(seq & 0xFF);  /* seq_lo */
    buf[3] = 0;
    /* CRC-16 CCITT over header */
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 4; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

static void build_pong_packet(uint8_t *buf, uint32_t seq)
{
    buf[0] = 0x02;  /* type = PONG */
    buf[1] = (uint8_t)(seq >> 8);    /* seq_hi */
    buf[2] = (uint8_t)(seq & 0xFF);  /* seq_lo */
    buf[3] = 0;
    /* CRC-16 CCITT over header */
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 4; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== PingPong 通知回调 ==================== */
static void on_pingpong_notify(ping_pong_t *pp, const ping_pong_notify_t *notify,
                               void *user_data)
{
    (void)user_data;
    switch (notify->type) {
        case PING_PONG_NOTIFY_TX_REQUEST:
            /* 根据角色填充不同包类型 */
            if (ping_pong_get_role(pp) == PING_PONG_ROLE_MASTER) {
                build_ping_packet(notify->payload.tx_request.tx_buffer, notify->seq);
            } else {
                build_pong_packet(notify->payload.tx_request.tx_buffer, notify->seq);
            }
            hal_radio_send(notify->payload.tx_request.tx_buffer,
                           notify->payload.tx_request.tx_buffer_size);
            break;

        case PING_PONG_NOTIFY_RX_REQUEST:
            hal_radio_start_rx();
            break;

        case PING_PONG_NOTIFY_SUCCESS:
            /* Master: 成功 → 开启下一轮 */
            if (ping_pong_get_role(pp) == PING_PONG_ROLE_MASTER) {
                pending_flags.restart_master = 1;
            }
            break;

        case PING_PONG_NOTIFY_FAIL:
            /* Master: 失败 → 也开启下一轮 */
            if (ping_pong_get_role(pp) == PING_PONG_ROLE_MASTER) {
                pending_flags.restart_master = 1;
            }
            break;

        case PING_PONG_NOTIFY_RX_TIMEOUT:
            /* Master/Slave: 接收超时 */
            if (ping_pong_get_role(pp) == PING_PONG_ROLE_SLAVE) {
                pending_flags.restart_slave = 1;
            }
            break;

        case PING_PONG_NOTIFY_RX_PING:
            /* Slave: 收到 Ping，可更新 LED 或日志 */
            break;

        case PING_PONG_NOTIFY_RX_PONG:
            /* Master: 收到 Pong，可更新 LED 或日志 */
            break;

        default:
            break;
    }
}

/* ==================== 主循环中处理重启 ==================== */
static void handle_pending_restarts(void)
{
    if (pending_flags.restart_master) {
        pending_flags.restart_master = 0;
        ping_pong_stop(&app.pp);
        ping_pong_start(&app.pp, PING_PONG_ROLE_MASTER);
    }
    if (pending_flags.restart_slave) {
        pending_flags.restart_slave = 0;
        ping_pong_stop(&app.pp);
        ping_pong_start(&app.pp, PING_PONG_ROLE_SLAVE);
    }
}

/* ==================== Radio 事件处理 ==================== */
static void handle_radio_events(void)
{
    if (hal_radio_tx_done()) {
        hal_radio_clear_tx_done();
        ping_pong_on_tx_done(&app.pp);
    }
    
    if (hal_radio_rx_done()) {
        uint32_t len;
        const uint8_t *data = hal_radio_get_rx_data(&len);
        int16_t rssi = hal_radio_get_rssi();
        int16_t snr = hal_radio_get_snr();
        hal_radio_clear_rx_done();
        ping_pong_on_rx_done(&app.pp, data, len, rssi, snr);
    }
}

/* ==================== 初始化 ==================== */
static void app_init(void)
{
    ping_pong_port_t port = {
        .get_time_ms = hal_get_tick_ms,
        .notify = on_pingpong_notify,
        .user_data = NULL
    };
    
    ping_pong_config_t config = {
        .timeout_ms = 100,           /* Master 超时 100ms */
        .max_retries = 3,            /* 最大重传 3 次 */
        .tx_buffer_size = sizeof(app.tx_buf),
        .slave_rx_timeout_ms = 5000  /* Slave 5秒无 Ping 则超时 */
    };
    
    ping_pong_init(&app.pp, &port);
    ping_pong_set_config(&app.pp, &config);
}

/* ==================== 主函数 ==================== */
int main(void)
{
    app_init();
    
    /* 根据角色启动（可通过宏或运行时配置） */
#ifdef CONFIG_ROLE_MASTER
    ping_pong_start(&app.pp, PING_PONG_ROLE_MASTER);
#else
    ping_pong_start(&app.pp, PING_PONG_ROLE_SLAVE);
#endif
    
    while (1) {
        ping_pong_process(&app.pp);      /* 超时检测 */
        handle_radio_events();            /* 硬件事件 */
        handle_pending_restarts();        /* 安全重启 */
    }
}