#pragma once
#include "esp_err.h"

// 初始化 I2C 总线、扫描设备、配置 PCM1865 为 TDM 4 通道从机(BCK-PLL)。
esp_err_t pcm1865_init(void);

// 扫描 I2C 总线并打印应答地址 (确认 PCM1865 实际地址)。
void pcm1865_scan(void);

// 回读时钟/PLL/供电状态寄存器, 诊断全零问题。须在 I2S 使能后调用。
void pcm1865_dump_status(void);
