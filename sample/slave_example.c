/*
 * PingPong 从机端接入示例
 *
 * 接入步骤：
 *  1. 实现 platform_get_time_ms()、radio_start_tx()、radio_start_rx()
 *  2. 系统初始化时调用 slave_init()
 *  3. 主循环中周期调用 slave_process()（间隔 ≤ 10 ms）
 *  4. 无线发送完成后（中断或回调）调用 slave_on_radio_tx_done()
 *  5. 无线接收完成后（中断或回调）调用 slave_on_radio_rx_done()
 */

/*============================ INCLUDES ======================================*/

#include "ping_pong.h"

/*============================ MACROS ========================================*/

/* SLAVE_RX_TIMEOUT_MS = 0 表示永不超时，持续监听。 */
#define SLAVE_RX_TIMEOUT_MS   0

/*============================ MACROFIED FUNCTIONS ===========================*/

#define g_slave ((ping_pong_t *)g_slave_mem)

/*============================ TYPES =========================================*/

/* None. */

/*============================ GLOBAL VARIABLES ==============================*/

/* None. */

/*============================ LOCAL VARIABLES ===============================*/

static uint8_t g_slave_mem[256 + PING_PONG_TX_BUFFER_SIZE];

/*============================ PROTOTYPES ====================================*/

static uint32_t platform_get_time_ms(void);
static void radio_start_tx(const uint8_t *data, uint32_t len);
static void radio_start_rx(void);
static void slave_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                         void *user_data);
void slave_init(void);
void slave_process(void);
void slave_on_radio_tx_done(void);
void slave_on_radio_rx_done(const uint8_t *data, uint32_t len,
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
     *       发送完成后须调用 slave_on_radio_tx_done() */
    (void)data;
    (void)len;
}

static void radio_start_rx(void)
{
    /* TODO: 调用无线芯片 HAL 进入接收模式
     *       收到数据后须调用 slave_on_radio_rx_done() */
}

static void slave_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                         void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (n->type) {
    case PING_PONG_NOTIFY_TX_REQUEST:
        /* 核心已完成 Pong 包编码；tx_len 才是本次真实发送长度。 */
        radio_start_tx(n->payload.tx_request.tx_buffer,
                       n->payload.tx_request.tx_len);
        break;

    case PING_PONG_NOTIFY_RX_REQUEST:
        radio_start_rx();
        break;

    case PING_PONG_NOTIFY_RX_TIMEOUT:
        /* 模块会自动重进 RX_WAIT 并再次发出 RX_REQUEST。 */
        break;

    default:
        break;
    }
}

void slave_init(void)
{
    ping_pong_port_t port = {
        .get_time_ms = platform_get_time_ms,
        .notify      = slave_notify,
        .user_data   = NULL,
        .trace       = NULL,
    };
    ping_pong_config_t config = {
        .max_retries   = 0,
        .rx_timeout_ms = SLAVE_RX_TIMEOUT_MS,
        .tx_timeout_ms = 0,
    };

    ping_pong_init(g_slave, &port);
    ping_pong_set_config(g_slave, &config);
    ping_pong_start(g_slave, PING_PONG_ROLE_SLAVE);
}

void slave_process(void)
{
    ping_pong_process(g_slave);
}

void slave_on_radio_tx_done(void)
{
    ping_pong_on_tx_done(g_slave);
}

void slave_on_radio_rx_done(const uint8_t *data, uint32_t len,
                            int16_t rssi, int16_t snr)
{
    ping_pong_on_rx_done(g_slave, data, len, rssi, snr);
}

int main(void)
{
    slave_init();
    for (;;) {
        slave_process();
    }
}
