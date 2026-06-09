// ============================================================================
//  网络流 —— 从 StreamBuffer 取数据, 按帧加头, TCP 发往目标 IP; 断线自动重连
//  帧格式 (小端):
//    magic[4]='A','4','C','H' | ver(1) | ch(1) | bits(1) | rsv(1)
//    sample_rate(4) | seq(4) | n_samples(4) | payload(交织16bit*ch*n_samples)
// ============================================================================
#include "net_stream.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "net";

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  channels;
    uint8_t  bits;
    uint8_t  reserved;
    uint32_t sample_rate;
    uint32_t seq;
    uint32_t n_samples;
} frame_hdr_t;   // 20 字节

static StreamBufferHandle_t s_sb;

size_t net_stream_write(const void *data, size_t len)
{
    // 0 超时: 缓冲满则丢新数据, 避免阻塞 I2S 读取
    return xStreamBufferSend(s_sb, data, len, 0);
}

static int connect_server(void)
{
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SERVER_PORT);
    inet_aton(SERVER_IP, &dst.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { ESP_LOGE(TAG, "socket() 失败 errno=%d", errno); return -1; }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGW(TAG, "connect %s:%d 失败 errno=%d", SERVER_IP, SERVER_PORT, errno);
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "已连接服务器 %s:%d", SERVER_IP, SERVER_PORT);
    return sock;
}

static int send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t left = len;
    while (left) {
        int n = send(sock, p, left, 0);
        if (n <= 0) return -1;
        p += n; left -= n;
    }
    return 0;
}

static void net_task(void *arg)
{
    uint8_t *payload = malloc(FRAME_BYTES);
    configASSERT(payload);
    int sock = -1;
    uint32_t seq = 0;
    uint32_t dropped_report = 0;

    while (1) {
        if (sock < 0) {
            sock = connect_server();
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            seq = 0;
            xStreamBufferReset(s_sb);   // 丢弃断线期间堆积的旧数据
        }

        size_t n = xStreamBufferReceive(s_sb, payload, FRAME_BYTES, pdMS_TO_TICKS(1000));
        if (n == 0) continue;

        frame_hdr_t h = {
            .magic = { 'A', '4', 'C', 'H' },
            .version = 1,
            .channels = NUM_CHANNELS,
            .bits = BITS_PER_SAMPLE,
            .reserved = 0,
            .sample_rate = SAMPLE_RATE,
            .seq = seq++,
            .n_samples = n / (NUM_CHANNELS * BYTES_PER_SAMPLE),
        };

        if (send_all(sock, &h, sizeof(h)) != 0 || send_all(sock, payload, n) != 0) {
            ESP_LOGW(TAG, "发送失败, 重连");
            close(sock);
            sock = -1;
        }

        if (++dropped_report >= 250) {  // 每 ~5s 报一次缓冲水位
            dropped_report = 0;
            ESP_LOGI(TAG, "缓冲剩余空间 %u 字节", (unsigned)xStreamBufferSpacesAvailable(s_sb));
        }
    }
}

void net_stream_init(void)
{
    s_sb = xStreamBufferCreate(STREAM_BUF_BYTES, FRAME_BYTES);
    configASSERT(s_sb);
    xTaskCreate(net_task, "net_task", 4096, NULL, 5, NULL);
}
