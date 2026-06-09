#pragma once
#include <stddef.h>
#include "esp_err.h"

// 初始化 I2S 为 TDM 4 通道 RX 主机 (ESP32 出 BCK/WS, 收 DIN)。
esp_err_t i2s_capture_init(void);

// 阻塞读取 I2S 数据。读出为 16-bit 通道交织: [s0c0 s0c1 s0c2 s0c3 s1c0 ...]
esp_err_t i2s_capture_read(void *buf, size_t len, size_t *bytes_read);

// 关闭再开启 I2S, 使 BCK/LRCK 停顿>3拍再恢复 —— 触发 PCM1865 从机自动时钟重检测。
// 仅在 i2s_capture_init 之后、i2s 读取任务尚未启动时调用。
esp_err_t i2s_capture_restart(void);
