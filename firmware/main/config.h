#pragma once
// ============================================================================
//  全局配置 —— 烧录前按现场修改 WiFi 与目标服务器
// ============================================================================

// ---- WiFi STA ----
#define WIFI_SSID        "xiaomiliangjin"
#define WIFI_PASS        "126password"
#define WIFI_MAX_RETRY   8            // 连续失败次数上限后重置重连

// ---- 目标服务器 (运行 server/recv_server.py 的设备) ----
#define SERVER_IP        "192.168.31.188"
#define SERVER_PORT      9000

// ---- 音频参数 ----
#define SAMPLE_RATE      48000        // Path B(BCK-PLL) 已支持 16000/48000; 改值需配 pcm1865.c PLL 系数表
#define NUM_CHANNELS     4            // PCM1865 TDM 4 路
#define BITS_PER_SAMPLE  16           // 固定 16-bit -> BCK = 64*fs (命中 PCM1865 BCK-PLL 工作点)

// ---- I2S 引脚 (ESP32-C3 = I2S 主机, 输出 BCK/WS, 接收 DIN) ----
#define I2S_BCK_GPIO     GPIO_NUM_2   // BCK  -> PCM1865 BCK
#define I2S_WS_GPIO      GPIO_NUM_3   // LRCK -> PCM1865 LRCK
#define I2S_DIN_GPIO     GPIO_NUM_1   // DOUT <- PCM1865 DOUT (TDM 单线 4 通道)

// ---- 时钟方案 (关键!) ----
// 全零静音的根因: 无 MCLK 时 PCM1865 没有系统时钟, ADC 不工作, 数字输出恒 0。
//   USE_MCLK_OUTPUT = 1 (推荐): ESP32 额外输出 256fs MCLK 到 PCM1865 SCK 脚,
//       需多接 1 根线 (I2S_MCLK_GPIO -> PCM1865 SCK/XI), 配 CLK_CTRL=0x00 自动检测。
//   USE_MCLK_OUTPUT = 0: 走 BCK-PLL (免线), 但需在 pcm1865.c 手填 PLL 寄存器, 难调。
// 本板 SCK 脚未引出 -> 用 Path B (BCK-PLL, 免线)。若日后能接 SCK 脚再改回 1。
#define USE_MCLK_OUTPUT  0
#define I2S_MCLK_GPIO    GPIO_NUM_10  // MCLK 输出 -> PCM1865 SCK/XI (仅 USE_MCLK_OUTPUT=1 时用)

// ---- I2C 引脚 (ESP32-C3 = 主机) ----
// 注意: GPIO8/9 是 strapping 脚, 见 docs/PLAN.md §1.1
#define I2C_SDA_GPIO     GPIO_NUM_8
#define I2C_SCL_GPIO     GPIO_NUM_9
#define I2C_FREQ_HZ      100000
#define PCM1865_I2C_ADDR 0x4A         // 随 ADR 引脚为 0x4A 或 0x4B, 启动扫描会确认

// ---- 分帧 / 缓冲 ----
#define FRAME_MS         20
#define FRAME_SAMPLES    (SAMPLE_RATE * FRAME_MS / 1000)              // 每通道样本数
#define BYTES_PER_SAMPLE (BITS_PER_SAMPLE / 8)
#define FRAME_BYTES      (FRAME_SAMPLES * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define STREAM_BUF_BYTES (FRAME_BYTES * 16)                           // 约 320ms 缓冲, 满则丢新帧
