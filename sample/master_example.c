/*
 * PingPong 主机端接入示例
 *
 * 接入步骤：
 *  1. 实现 platform_get_time_ms()、radio_start_tx()、radio_start_rx()
 *  2. 系统初始化时调用 master_init()
 *  3. 主循环中周期调用 master_process()（间隔 ≤ 10 ms）
 *  4. 无线发送完成后（中断或回调）调用 master_on_radio_tx_done()
 *  5. 无线接收完成后（中断或回调）调用 master_on_radio_rx_done()
 */

/*============================ INCLUDES ======================================*/

#include "ping_pong.h"

/*============================ MACROS ========================================*/

#define MASTER_TIMEOUT_MS      3000
#define MASTER_MAX_RETRIES        3

/*============================ MACROFIED FUNCTIONS ===========================*/

#define g_master ((ping_pong_t *)g_master_mem)

/*============================ TYPES =========================================*/

/* None. */

/*============================ GLOBAL VARIABLES ==============================*/

/* None. */

/*============================ LOCAL VARIABLES ===============================*/

static uint8_t g_master_mem[256 + PING_PONG_TX_BUFFER_SIZE];
static volatile int g_restart_pending;

/*============================ PROTOTYPES ====================================*/

static uint32_t platform_get_time_ms(void);
static void radio_start_tx(const uint8_t *data, uint32_t len);
static void radio_start_rx(void);
static void master_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                          void *user_data);
void master_init(void);
void master_process(void);
void master_on_radio_tx_done(void);
void master_on_radio_rx_done(const uint8_t *data, uint32_t len,
                             int16_t rssi, int16_t snr);

/*============================ IMPLEMENTATION ================================*/

static uint32_t platform_get_time_ms(void)
{
    /* TODO: 替换为平台 SysTick 或 RTC 计数 */
    return 0;
}

static void radio_start_tx(const uint8_t *data, uint32_t len)
{
    /* TODO: 调用无线芯片 HAL 发送
     *       发送完成后须调用 master_on_radio_tx_done() */
    (void)data;
    (void)len;
}

static void radio_start_rx(void)
{
    /* TODO: 调用无线芯片 HAL 进入接收模式
     *       收到数据后须调用 master_on_radio_rx_done() */
}

static void master_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                          void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (n->type) {
    case PING_PONG_NOTIFY_TX_REQUEST:
        /* 核心已完成 Ping 包编码；tx_len 才是本次真实发送长度。 */
        radio_start_tx(n->payload.tx_request.tx_buffer,
                       n->payload.tx_request.tx_len);
        break;

    case PING_PONG_NOTIFY_RX_REQUEST:
        radio_start_rx();
        break;

    case PING_PONG_NOTIFY_SUCCESS:
    case PING_PONG_NOTIFY_FAIL:
        g_restart_pending = 1;
        break;

    default:
        break;
    }
}

void master_init(void)
{
    ping_pong_port_t port = {
        .get_time_ms = platform_get_time_ms,
        .notify      = master_notify,
        .user_data   = NULL,
        .trace       = NULL,
    };
    ping_pong_config_t config = {
        .max_retries   = MASTER_MAX_RETRIES,
        .rx_timeout_ms = MASTER_TIMEOUT_MS,
        .tx_timeout_ms = 0,
    };

    ping_pong_init(g_master, &port);
    ping_pong_set_config(g_master, &config);
    ping_pong_start(g_master, PING_PONG_ROLE_MASTER);
}

void master_process(void)
{
    ping_pong_process(g_master);

    if (g_restart_pending) {
        g_restart_pending = 0;
        ping_pong_start(g_master, PING_PONG_ROLE_MASTER);
    }
}

void master_on_radio_tx_done(void)
{
    ping_pong_on_tx_done(g_master);
}

void master_on_radio_rx_done(const uint8_t *data, uint32_t len,
                             int16_t rssi, int16_t snr)
{
    ping_pong_on_rx_done(g_master, data, len, rssi, snr);
}

int main(void)
{
    master_init();
    for (;;) {
        master_process();
    }
}
