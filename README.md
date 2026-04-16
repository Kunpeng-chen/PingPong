# PingPong

[![CI](https://github.com/Kunpeng-chen/PingPong/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/Kunpeng-chen/PingPong/actions/workflows/ci.yml)

面向嵌入式场景的轻量级 Ping-Pong 协议核心模块。它只负责协议状态机、收发时序、超时与重试、基础统计和通知回调；时钟、无线收发、日志和业务决策都交给上层端口适配。

## Highlights

- 核心代码集中在 `src/ping_pong.h` 和 `src/ping_pong.c`
- 通过端口回调接入平台时间、收发和通知能力
- 支持 `MASTER` / `SLAVE` 两种运行角色
- 内建超时、重试和冲突检测
- 提供基础统计，方便接入现有工程和调试链路

## Quick Start

1. 在你的平台层实现时间获取和无线收发回调。
2. 分配实例内存并调用 `ping_pong_init()`。
3. 按需调用 `ping_pong_set_config()` 覆盖默认超时与重试参数。
4. 使用 `ping_pong_start()` 选择 `MASTER` 或 `SLAVE`。
5. 在主循环里周期调用 `ping_pong_process()`，并在底层收发完成后回调 `ping_pong_on_tx_done()` / `ping_pong_on_rx_done()`。

## Docs

- [详细设计与接口说明](src/README.md)
- [主机端示例](sample/master_example.c)
- [从机端示例](sample/slave_example.c)
- [单元测试](test/test_ping_pong.c)

如果你想看状态机、包格式、错误码和设计边界，请直接阅读 [src/README.md](src/README.md)。

## Repository Map

- `src/` 核心协议实现与接口
- `sample/` 接入示例
- `test/` 单元测试
- `.github/workflows/ci.yml` 持续集成
