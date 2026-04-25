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

#include "ping_pong.h"

/* ==================== 配置 ==================== */

#define MASTER_TIMEOUT_MS      3000
#define MASTER_MAX_RETRIES        3

/* ==================== 实例内存 ==================== */

/* 内存大小 = 内部结构体（≤ 256 字节）+ 发送缓冲区 */
static uint8_t g_master_mem[256 + PING_PONG_TX_BUFFER_SIZE];
#define g_master ((ping_pong_t *)g_master_mem)

/* 延迟重启标志：SUCCESS/FAIL 回调中置位，主循环中处理，避免重入 */
static volatile int g_restart_pending;

/* ==================== 平台适配 [需实现] ==================== */

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

/* ==================== Ping 包编码 ==================== */

/*
 * 包格式（6 字节）：
 *   [0]     type     = 0x01 (PING)
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

static void encode_ping(uint8_t *buf, uint16_t seq)
{
    uint16_t crc;
    buf[0] = 0x01;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = 0;
    crc    = crc16_ccitt(buf, 4);
    buf[4] = (uint8_t)(crc >> 8);
    buf[5] = (uint8_t)(crc & 0xFF);
}

/* ==================== 通知回调 ==================== */

static void master_notify(ping_pong_t *pp, const ping_pong_notify_t *n,
                          void *user_data)
{
    (void)pp;
    (void)user_data;

    switch (n->type) {

    case PING_PONG_NOTIFY_TX_REQUEST:
        /* 将 Ping 包写入模块提供的发送缓冲区，然后启动无线发送 */
        encode_ping(n->payload.tx_request.tx_buffer, (uint16_t)n->seq);
        radio_start_tx(n->payload.tx_request.tx_buffer,
                       n->payload.tx_request.tx_buffer_size);
        break;

    case PING_PONG_NOTIFY_RX_REQUEST:
        /* 模块已进入等待状态，启动无线接收 */
        radio_start_rx();
        break;

    case PING_PONG_NOTIFY_SUCCESS:
        /* 本轮成功，延迟到主循环重启下一轮 */
        g_restart_pending = 1;
        break;

    case PING_PONG_NOTIFY_FAIL:
        /* 本轮失败（超时/重传耗尽/CRC 错误等），延迟到主循环重启 */
        g_restart_pending = 1;
        break;

    default:
        break;
    }
}

/* ==================== 对外接口 ==================== */

void master_init(void)
{
    ping_pong_port_t port = {
        .get_time_ms = platform_get_time_ms,
        .notify      = master_notify,
        .user_data   = NULL,
        .trace       = NULL,
    };
    /* 默认配置已在 init 中填充（rx_timeout_ms=3000, max_retries=3, tx_timeout_ms=3000），
     * 此处覆盖为项目专属值。 */
    ping_pong_config_t config = {
        .max_retries   = MASTER_MAX_RETRIES,
        .rx_timeout_ms = MASTER_TIMEOUT_MS,
        .tx_timeout_ms = 0,
    };

    ping_pong_init(g_master, &port);
    ping_pong_set_config(g_master, &config);
    ping_pong_start(g_master, PING_PONG_ROLE_MASTER);
}

/* 在主循环中周期调用，建议间隔 ≤ 10 ms */
void master_process(void)
{
    ping_pong_process(g_master);

    if (g_restart_pending) {
        g_restart_pending = 0;
        /* MASTER → MASTER 重启时模块自动递增 seq */
        ping_pong_start(g_master, PING_PONG_ROLE_MASTER);
    }
}

/* 无线发送完成中断/回调中调用 */
void master_on_radio_tx_done(void)
{
    ping_pong_on_tx_done(g_master);
}

/* 无线接收完成中断/回调中调用 */
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