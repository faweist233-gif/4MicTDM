// ============================================================================
//  ESP32-C3 四声道(PCM1865 TDM) WiFi 音频采集 —— 主程序
//
//  流水线: PCM1865 --TDM--> I2S(RX master) --DMA--> i2s_task
//          --> StreamBuffer --> net_task --TCP--> 目标 IP
// ============================================================================
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "config.h"
#include "wifi_sta.h"
#include "pcm1865.h"
#include "i2s_capture.h"
#include "net_stream.h"

static const char *TAG = "main";

// 周期性打印通道电平, 便于无服务器时本地确认采集是否正常
static void log_levels(const int16_t *s, size_t frames)
{
    int64_t acc[NUM_CHANNELS] = { 0 };
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < NUM_CHANNELS; c++) {
            int v = s[i * NUM_CHANNELS + c];
            acc[c] += (int64_t)v * v;
        }
    ESP_LOGI(TAG, "RMS ch0=%lld ch1=%lld ch2=%lld ch3=%lld",
             (long long)(acc[0] / (frames ? frames : 1)),
             (long long)(acc[1] / (frames ? frames : 1)),
             (long long)(acc[2] / (frames ? frames : 1)),
             (long long)(acc[3] / (frames ? frames : 1)));
}

static void i2s_task(void *arg)
{
    uint8_t *buf = malloc(FRAME_BYTES);
    configASSERT(buf);
    uint32_t cnt = 0;

    while (1) {
        size_t got = 0;
        if (i2s_capture_read(buf, FRAME_BYTES, &got) != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        net_stream_write(buf, got);

        if (++cnt >= (1000 / FRAME_MS)) {   // 约每秒
            cnt = 0;
            log_levels((const int16_t *)buf, got / (NUM_CHANNELS * BYTES_PER_SAMPLE));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "启动: 4ch TDM @ %d Hz -> %s:%d", SAMPLE_RATE, SERVER_IP, SERVER_PORT);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_sta_connect_blocking();

    // 顺序关键: 先启动 I2S, 让 ESP32 立即输出 BCK/LRCK;
    // 再配置 PCM1865 —— 这样 PLL 的自动时钟检测器在配置时已能看到 BCK, 才能锁定。
    // (反过来先配 PCM1865 会因无 BCK 锁不上 PLL, 之后不自动重锁 -> 全零静音)
    ESP_ERROR_CHECK(i2s_capture_init());
    vTaskDelay(pdMS_TO_TICKS(20));      // 等 BCK/LRCK 稳定
    ESP_ERROR_CHECK(pcm1865_init());    // 内部会等待并轮询 PLL 锁定

    pcm1865_dump_status();

    net_stream_init();

    xTaskCreate(i2s_task, "i2s_task", 4096, NULL, 6, NULL);
}
