/* 调用者分配上下文（大小由实现定义，建议 >= 256 字节） */
static uint8_t ping_pong_ctx[256];

/* 实现时间函数 */
static uint32_t my_get_time_ms(void)
{
    return hal_get_tick_ms();
}

/* 实现通知回调 */
static void my_notify(void *ctx, const ping_pong_notify_t *notify)
{
    switch (notify->type) {
    case PING_PONG_NOTIFY_TX_REQUEST:
        radio_send(notify->seq, ping_pong_get_tx_buffer(ctx));
        break;
    case PING_PONG_NOTIFY_RX_REQUEST:
        radio_enter_rx_mode();
        break;
    case PING_PONG_NOTIFY_SUCCESS:
        LOG("Ping-Pong success, RTT=%d ms", notify->payload.success.rtt_ms);
        event_bus_publish(EVENT_PING_SUCCESS, &notify->payload.success);
        break;
    /* ... 其他通知处理 ... */
    }
}

/* 端口配置 */
static const ping_pong_port_t my_port = {
    .get_time_ms = my_get_time_ms,
    .notify = my_notify,
};

/* 使用 */
void main(void)
{
    ping_pong_init(ping_pong_ctx, NULL, &my_port);
    ping_pong_start(ping_pong_ctx, PING_PONG_ROLE_MASTER);
    
    while (1) {
        ping_pong_process(ping_pong_ctx);
        /* ... */
    }
}