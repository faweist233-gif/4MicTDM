# ESP32-C3 四声道(PCM1865 TDM)音频 WiFi 采集系统 — 方案文档

> 目标：ESP32-C3 通过 I2C 配置 PCM1865，经 I2S/TDM 采集 4 路音频，再用 WiFi 将 4 通道实时数据发送到目标 IP 设备；交付**烧录固件**与**服务器端接收程序**。

---

## 1. 硬件接线（已定稿）

| 信号 | PCM1865 | ESP32-C3 | 方向 | 说明 |
|------|---------|----------|------|------|
| DOUT | DOUT    | GPIO1    | PCM1865 → ESP32 | I2S 数据，TDM 单线承载 4 通道 |
| BCK  | BCK     | GPIO2    | **ESP32 → PCM1865** | 位时钟，**ESP32 主机输出** |
| LRCK | LRCK    | GPIO3    | **ESP32 → PCM1865** | 帧时钟，**ESP32 主机输出** |
| SDA  | SDA     | **GPIO8** | 双向 | I2C 控制（已定，注意 strapping）|
| SCL  | SCL     | **GPIO9** | ESP32 → PCM1865 | I2C 时钟（已定，注意 strapping）|

> ✅ **不需要 MCLK 线**：见 §1.1。ESP32 仅输出 BCK + LRCK，PCM1865 从 BCK 派生系统时钟。

### 1.1 时钟主从拓扑（已确认）

> 结论：**ESP32-C3 = I2S 主机(RX master)** 生成 BCK + LRCK；**PCM1865 = 从机**，用内部 PLL **以 BCK 作参考时钟**生成自身系统时钟（无需晶振、无需 MCLK 线）。3 根音频线即可。

- PCM1865 无独立晶振 → 不能做主机，也不能等外部 MCLK；
- PCM1865 支持「slave + PLL with BCK reference」模式：靠 ESP32 提供的 BCK 经内部 PLL 生成 MCLK（寄存器 `CLK_MODE` Page0 0x20 + PLL 配置）；
- TI 文档化的工作点：**BCK = 64·fs**。本方案默认 **16-bit slot × 4 通道 = 64·fs**，正好命中该工作点 —— 这是把默认位深定为 16-bit 的硬约束理由之一。
- 若改 24-bit（slot=32-bit）→ BCK=128·fs，需另行验证 PLL 是否接受该参考频率。

#### GPIO 选择注意（ESP32-C3 strapping 脚）

- ESP32-C3 的 I2C/I2S **无固定引脚**，经 GPIO 矩阵可路由到任意 GPIO；
- **I2C 实接 GPIO8(SDA)/GPIO9(SCL)** —— 两者均为 strapping 脚：GPIO8 上电须为高（否则进下载模式），GPIO9 是 BOOT 键脚。PCM1865 的 I2C 是开漏 + 上拉，上电默认拉高，通常不影响启动；若烧录/复位异常，先断开 I2C 上拉或确认 PCM1865 不在上电瞬间拉低这两脚。
- **GPIO2 也是 strapping 脚**（BCK 用了它）：由 ESP32 输出驱动，复位采样后即正常；PCM1865 的 BCK 输入是高阻，通常无碍。异常则把 BCK 换到 GPIO6/7。

---

## 2. 数据格式与带宽预算

- **通道数**：4（PCM1865 双 ADC，TDM 模式单线输出 4 slot）
- **位深**：默认 16-bit（PCM1865 支持到 24-bit，TDM slot 宽 32-bit）
- **采样率**：默认 16 kHz，可配 8/16/32/48 kHz

| 配置 | 网络净荷码率 | 评估 |
|------|-------------|------|
| 4ch×16bit×16k | ≈1.02 Mbit/s | 轻松 |
| 4ch×16bit×48k | ≈3.07 Mbit/s | 可用 |
| 4ch×24bit×48k | ≈4.6 Mbit/s | 接近 C3 TCP 实测上限，需加大缓冲 |

> ESP32-C3 单核 RISC-V @160MHz，WiFi TCP 实测吞吐约 10–20 Mbit/s，故上述配置均可行；高码率需关注 DMA/环形缓冲不溢出。

---

## 3. 软件架构

### 3.1 固件（ESP-IDF，新版 I2S 驱动）

```
[PCM1865(slave)] --TDM/I2S(DOUT)--> [ESP32-C3 I2S RX(master, 出BCK/LRCK) + DMA]
                              |
                       i2s_read 任务 ──> [环形缓冲 ringbuf]
                              |
                       net_send 任务 ──> [TCP client] --WiFi--> 目标IP:PORT
        I2C(一次性) ──> 配置 PCM1865 寄存器(TDM/采样率/增益)
```

任务划分（FreeRTOS）：
- **i2s_rx_task**：`i2s_channel_read()` 持续读 DMA → 写入 ringbuf；
- **net_task**：从 ringbuf 取数据 → 加帧头 → `send()` 到 TCP；断线自动重连；
- **wifi event**：STA 连接/重连、IP 获取后启动 net_task。

ESP-IDF 要点：
- 用新版 `driver/i2s_tdm.h`，`i2s_channel_init_tdm_mode()`，**`role = I2S_ROLE_MASTER`**（ESP32 出 BCK/LRCK）；
- slot_mask = SLOT0|SLOT1|SLOT2|SLOT3（4 通道），slot_bit_width = 16 → BCK=64·fs；
- MCLK 不输出（`mclk = I2S_GPIO_UNUSED`），PCM1865 自己用 BCK 派生时钟；
- DMA：`dma_desc_num=6, dma_frame_num=240` 起步，按掉帧情况调。

### 3.2 PCM1865 I2C 配置（关键寄存器）

> 7-bit I2C 地址默认 **0x4A**（随 ADR 引脚变 0x4A/0x4B），需用逻辑分析仪/扫描确认。

需配置项（分页寄存器，具体值在 `pcm1865.c` 注释中给出）：
1. ADC 输入通道选择（CH1L/R、CH2L/R → 对应 4 路麦克风）；
2. 格式寄存器 FMT：选 **TDM 模式**、字长 16-bit（slot=16，对齐 BCK=64·fs）；
3. TDM slot 偏移：4 通道分别落在 slot 0/1/2/3（`TX_TDM_OFFSET`）；
4. **时钟模式：PCM1865 设为 slave + BCK-PLL** —— `CLK_MODE`(Page0 0x20) 选 PLL 参考来自 BCK，配置 PLL 系数使内部 MCLK = 目标 fs 的 256/384·fs；从机不输出 BCK/LRCK；
5. 增益/PGA、高通滤波器等按需。

### 3.3 网络传输协议（自定义轻量帧）

默认 **TCP**（采集完整性优先；附 UDP 备选）。每帧：

```
偏移  字段          长度   说明
0     magic         4B     'A','4','C','H' (0x41 0x34 0x43 0x48)
4     version       1B     0x01
5     channels      1B     4
6     bits          1B     16
7     reserved      1B     0
8     sample_rate   4B     LE, 如 16000
12    seq           4B     LE, 帧序号(检测丢包)
16    n_samples     4B     LE, 本帧每通道样本数
20    payload       N      交织样本: [s0c0 s0c1 s0c2 s0c3 s1c0 ...]
```

- payload 为**通道交织**的小端 PCM；
- 每帧承载约 10–20 ms 数据（降低开销同时控制延迟）；
- UDP 备选：同帧格式，靠 seq 检测丢包，适合低延迟可容忍丢包场景。

### 3.4 服务器端（Python）

- TCP 监听 → 接收解析帧 → 按 seq 检测丢包 → 反交织成 4 路 → 写 **4 通道 WAV**（`wave` 模块 `nchannels=4`，或拆 4 个单声道）；
- 可选实时：RMS/电平打印、简单波形可视化；
- 文件名：`rec_{YYYYMMDD_HHMMSS}_{sr}Hz_4ch.wav`。

---

## 4. 目录结构（拟）

```
4MicTDM/
├── docs/PLAN.md                  # 本文档
├── firmware/                     # ESP-IDF 工程
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults        # 启用新I2S/WiFi
│   └── main/
│       ├── CMakeLists.txt
│       ├── config.h              # 引脚/SSID/目标IP/采样率/角色开关
│       ├── main.c                # 初始化+任务创建
│       ├── pcm1865.c / .h        # I2C寄存器配置(TDM)
│       ├── i2s_capture.c / .h    # I2S TDM RX + ringbuf
│       ├── wifi_sta.c / .h       # WiFi STA连接/重连
│       └── net_stream.c / .h     # TCP client + 帧打包
└── server/
    ├── recv_server.py            # TCP接收+解析+存WAV
    ├── requirements.txt          # numpy(可选可视化)
    └── README.md                 # 运行说明
```

---

## 5. 实施里程碑

| 阶段 | 内容 | 验收 |
|------|------|------|
| M0 | ✅ 时钟拓扑已定(ESP32主机/PCM1865 BCK-PLL从机)；剩 I2C 地址扫描确认 | 接线表定稿 |
| M1 | PCM1865 I2C 配置 + I2S TDM 读 | 串口打印 4 通道电平随声音变化 |
| M2 | WiFi STA + TCP 帧发送 | 服务器收到帧、seq 连续 |
| M3 | 服务器存 4ch WAV + 丢包统计 | 回放 4 路对应 4 麦 |
| M4 | 稳定性/重连/高码率压测 | 长跑无溢出、断线自恢复 |

---

## 6. 风险与待确认项

1. ~~时钟主从~~ ✅ 已定：ESP32 主机 / PCM1865 BCK-PLL 从机，无 MCLK 线。
2. **I2C 地址**：默认 0x4A（随 ADR 脚 0x4A/0x4B），固件启动先做 I2C 扫描打印确认。引脚已定 GPIO4/5。
3. **BCK-PLL 锁定**：PCM1865 PLL 以 BCK 为参考需 BCK 稳定且为 64·fs；若改 24-bit(128·fs) 需验证 PLL 接受范围。
4. **strapping 脚**：BCK 用了 GPIO2（strapping），I2C 已避开 GPIO8/9；如启动异常将 BCK 移到 GPIO6/7。
5. **ESP32-C3 TDM 支持**：C3 新 I2S 外设支持 TDM；需 ESP-IDF ≥ v5.0。
6. **高码率掉帧**：24bit/48k 接近上限，预留缓冲调参空间。
7. **WiFi 稳定性**：弱信号下 TCP 阻塞→缓冲堆积，需限缓冲上限并丢旧帧或断流告警。

---

## 7. 下一步

硬件拓扑与引脚已定稿，可直接生成 M1 固件骨架（含 I2C 扫描自检）+ 服务器脚本。I2C 地址在固件启动扫描时确认即可，不阻塞开发。
