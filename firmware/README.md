# 固件 — ESP32-C3 四声道 TDM 采集 + WiFi 推流

ESP-IDF 工程（需 **ESP-IDF ≥ v5.2**，新版 I2S/I2C 驱动）。

## 配置

编辑 [main/config.h](main/config.h)：
- `WIFI_SSID` / `WIFI_PASS`
- `SERVER_IP` / `SERVER_PORT`（运行 `server/recv_server.py` 的设备）
- 采样率 `SAMPLE_RATE`（默认 16000）

引脚（已按硬件固定，如需改在此处改）：

| 功能 | GPIO | 说明 |
|------|------|------|
| I2S DIN (PCM1865 DOUT) | IO1 | TDM 单线 4 通道 |
| I2S BCK | IO2 | ESP32 输出 |
| I2S LRCK | IO3 | ESP32 输出 |
| I2C SDA | IO8 | strapping 脚，注意上电电平 |
| I2C SCL | IO9 | strapping 脚 |

## 编译 / 烧录

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COM5 flash monitor      # 端口按实际改
```

## Bring-up 检查顺序

1. **I2C**：启动日志 `I2C 扫描` 应打印 `发现设备 @ 0x4A`（或 0x4B）。没有→查 SDA/SCL 接线与上拉，并据实改 `PCM1865_I2C_ADDR`。
2. **采集**：`monitor` 里每秒打印 `RMS ch0..ch3`，对麦克风讲话对应通道数值上升。全 0 →查 I2S 时序 / PCM1865 寄存器（见 [main/pcm1865.c](main/pcm1865.c) 注释）。
3. **时钟锁定**：若 RMS 抖动剧烈或全噪声，可能 BCK-PLL 未锁——试把 `pcm1865.c` 的 `CLK_CTRL` 改成自动时钟检测 `0x00`。
4. **通道错位**：若 4 路内容互相串/整体偏移，调 `PCM186X_TDM_TX_OFFSET`（0x0d）。
5. **网络**：服务器先启动；ESP32 日志出现 `已连接服务器`，服务器端出现 `[REC] 写入 ...`。

## 模块

| 文件 | 职责 |
|------|------|
| `main.c` | 初始化 + i2s_task（读 I2S → 写发送缓冲 + 电平日志）|
| `pcm1865.c` | I2C 配置 PCM1865（TDM/16-bit/slave BCK-PLL）|
| `i2s_capture.c` | I2S TDM RX 主机 |
| `wifi_sta.c` | WiFi STA 连接/重连 |
| `net_stream.c` | StreamBuffer → TCP 帧发送 + 断线重连 |
