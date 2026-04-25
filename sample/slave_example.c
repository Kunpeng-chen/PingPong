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

#include "ping_pong.h"

/* ==================== 配置 ==================== */

/*
 * SLAVE_RX_TIMEOUT_MS = 0  永不超时，持续监听
 * 非零值：超时后触发 PING_PONG_NOTIFY_RX_TIMEOUT，模块自动重进监听
 */
#define SLAVE_RX_TIMEOUT_MS   0

/* ==================== 实例内存 ==================== */

static uint8_t g_slave_mem[256 + PING_PONG_TX_BUFFER_SIZE];
#define g_slave ((ping_pong_t *)g_slave_mem)

/* ==================== 平台适配 [需实现] ==================== */

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

/* ==================== Pong 包编码 ==================== */

/*
 * 包格式（6 字节）：
 *   [0]     type     = 0x02 (PONG)
 *   [1]     seq_hi
 *   [2]     seq_lo
 *   [3]     reserved = 0x00
 *   [4..5]  CRC-16 CCITT（大端，覆盖 [0..3]）
 */

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
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

static void encode_pong(uint8_t *buf, uint16_t seq)
{
    uint16_t crc;
    buf[0] = 0x02;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    crc    = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== 通知回调 ==================== */

static void slave_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                         void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (n->type) {

    case PING_PONG_NOTIFY_TX_REQUEST:
        /* 将 Pong 包写入模块提供的发送缓冲区，然后启动无线发送 */
        encode_pong(n->payload.tx_request.tx_buffer, (uint16_t)n->seq);
        radio_start_tx(n->payload.tx_request.tx_buffer,
                       n->payload.tx_request.tx_buffer_size);
        break;

    case PING_PONG_NOTIFY_RX_REQUEST:
        /* 模块已切换到等待状态，启动无线接收 */
        radio_start_rx();
        break;

    case PING_PONG_NOTIFY_RX_TIMEOUT:
        /* 模块自动重进 RX_WAIT 并再次发出 RX_REQUEST
         * 此处可添加超时统计或告警逻辑 */
        break;

    default:
        break;
    }
}

/* ==================== 对外接口 ==================== */

void slave_init(void)
{
    ping_pong_port_t port = {
        .get_time_ms = platform_get_time_ms,
        .notify      = slave_notify,
        .user_data   = NULL,
        .trace       = NULL,
    };
    /*
     * Slave 无需设置 max_retries（仅 Master 使用）。
     * rx_timeout_ms=0 表示永不超时，持续监听。
     */
    ping_pong_config_t config = {
        .max_retries   = 0,
        .rx_timeout_ms = SLAVE_RX_TIMEOUT_MS,
        .tx_timeout_ms = 0,
    };

    ping_pong_init(g_slave, &port);
    ping_pong_set_config(g_slave, &config);
    ping_pong_start(g_slave, PING_PONG_ROLE_SLAVE);
}

/* 在主循环中周期调用，建议间隔 ≤ 10 ms */
void slave_process(void)
{
    ping_pong_process(g_slave);
    /* 无需手动重启：Slave 在 Pong 发完后自动回到 RX_WAIT */
}

/* 无线发送完成中断/回调中调用 */
void slave_on_radio_tx_done(void)
{
    ping_pong_on_tx_done(g_slave);
}

/* 无线接收完成中断/回调中调用 */
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
