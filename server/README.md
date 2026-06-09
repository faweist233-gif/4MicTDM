# 服务器端 — 4 声道音频接收

接收 ESP32-C3 (PCM1865 TDM) 经 WiFi/TCP 推来的 4 通道音频，存为 4 声道 WAV。

## 运行

```bash
cd server
python recv_server.py --host 0.0.0.0 --port 9000 --outdir ./recordings
```

- 仅用 Python 标准库，无需 `pip install`。
- 把固件 `config.h` 里的 `SERVER_IP` 改成本机 IP、`SERVER_PORT` 与 `--port` 一致。
- 每次 ESP32 连接生成一个文件：`recordings/rec_YYYYMMDD_HHMMSS_16000Hz_4ch.wav`。
- 终端打印连接状态、丢包统计、采样速率。

## 验证录音

WAV 为 4 通道交织。拆成单通道查看：

```bash
# ffmpeg 拆 4 路单声道
ffmpeg -i rec_xxx_4ch.wav -map_channel 0.0.0 ch0.wav \
                          -map_channel 0.0.1 ch1.wav \
                          -map_channel 0.0.2 ch2.wav \
                          -map_channel 0.0.3 ch3.wav
```

或用 Audacity 直接打开（显示 4 条轨）。

## 帧协议

| 偏移 | 字段 | 长度 | 说明 |
|------|------|------|------|
| 0 | magic | 4 | `'A','4','C','H'` |
| 4 | version | 1 | 0x01 |
| 5 | channels | 1 | 4 |
| 6 | bits | 1 | 16 |
| 7 | reserved | 1 | 0 |
| 8 | sample_rate | 4 | LE, 如 16000 |
| 12 | seq | 4 | LE, 帧序号 |
| 16 | n_samples | 4 | LE, 每通道样本数 |
| 20 | payload | N | 交织 int16: `s0c0 s0c1 s0c2 s0c3 s1c0 ...` |

服务器对 magic 做滑动重同步，丢包/错位后能自动重新对齐。
