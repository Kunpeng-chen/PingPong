/**
 * @file ping_pong_config.h
 * @brief Compile-time default configuration for PingPong.
 *
 * Users may override these macros from the build system or by providing
 * compiler definitions before including ping_pong.h. Runtime configuration can
 * still override these values through ping_pong_set_config().
 */

#ifndef PING_PONG_CONFIG_H
#define PING_PONG_CONFIG_H

#ifndef PING_PONG_DEFAULT_MAX_RETRIES
#define PING_PONG_DEFAULT_MAX_RETRIES       3u
#endif

#ifndef PING_PONG_DEFAULT_RX_TIMEOUT_MS
#define PING_PONG_DEFAULT_RX_TIMEOUT_MS  3000u
#endif

#ifndef PING_PONG_DEFAULT_TX_TIMEOUT_MS
#define PING_PONG_DEFAULT_TX_TIMEOUT_MS  3000u
#endif

#ifndef PING_PONG_DEFAULT_AUTO_RESTART
#define PING_PONG_DEFAULT_AUTO_RESTART      0u
#endif

#ifndef PING_PONG_DEFAULT_RESTART_DELAY_MS
#define PING_PONG_DEFAULT_RESTART_DELAY_MS  0u
#endif

#endif /* PING_PONG_CONFIG_H */
