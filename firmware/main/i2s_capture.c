// ============================================================================
//  I2S TDM 采集 —— ESP32-C3 作 I2S 主机, 4 槽 16-bit, 接收 PCM1865 DOUT
//  使用 ESP-IDF v5 新版 I2S 驱动 (driver/i2s_tdm.h)
// ============================================================================
#include "i2s_capture.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "i2s";
static i2s_chan_handle_t s_rx = NULL;

esp_err_t i2s_capture_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;       // 掉帧时增大
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new_channel");

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
#if USE_MCLK_OUTPUT
            .mclk = I2S_MCLK_GPIO,      // 输出 MCLK 给 PCM1865 系统时钟
#else
            .mclk = I2S_GPIO_UNUSED,    // BCK-PLL: 不输出 MCLK
#endif
            .bclk = I2S_BCK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
#if USE_MCLK_OUTPUT
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // MCLK = 256*fs, PCM1865 标准比率
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &tdm_cfg), TAG, "init_tdm");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");

    ESP_LOGI(TAG, "I2S TDM RX master 就绪: %d ch / 16-bit / fs=%d / BCK=64fs",
             NUM_CHANNELS, SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t i2s_capture_read(void *buf, size_t len, size_t *bytes_read)
{
    return i2s_channel_read(s_rx, buf, len, bytes_read, portMAX_DELAY);
}
