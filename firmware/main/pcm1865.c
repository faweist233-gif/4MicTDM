// ============================================================================
//  PCM1865 配置 (I2C)  —— TDM 4 通道, 从机模式, 时钟由 BCK 经内部 PLL 派生
//
//  寄存器地址/位域取自 TI PCM186x 数据手册 (SLAS831) 与 Linux 内核驱动
//  sound/soc/codecs/pcm186x.h。所有写值含义见注释。
//  ⚠ 现场 bring-up 必须用逻辑分析仪核对 DOUT 时序, 不对则按注释调 OFFSET/极性。
// ============================================================================
#include "pcm1865.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcm1865";

// ---- PCM186x 寄存器地址 (Page 0) ----
#define PCM186X_PAGE              0x00
#define PCM186X_PGA_VAL_CH1_L     0x01
#define PCM186X_PGA_VAL_CH1_R     0x02
#define PCM186X_PGA_VAL_CH2_L     0x03
#define PCM186X_PGA_VAL_CH2_R     0x04
#define PCM186X_PGA_CTRL          0x05
#define PCM186X_ADC1_INPUT_SEL_L  0x06
#define PCM186X_ADC1_INPUT_SEL_R  0x07
#define PCM186X_ADC2_INPUT_SEL_L  0x08
#define PCM186X_ADC2_INPUT_SEL_R  0x09
#define PCM186X_PCM_CFG           0x0b   // FMT(TDM/I2S) + 字长
#define PCM186X_TDM_TX_SEL        0x0c
#define PCM186X_TDM_TX_OFFSET     0x0d
#define PCM186X_CLK_CTRL          0x20   // 主从 / PLL 源 / SCK 源
#define PCM186X_POWER_CTRL        0x70

// ---- PCM_CFG 位 ----
// RX_WLEN[7:6], TX_WLEN[3:2], FMT[1:0]; 16-bit=0b11, TDM=0b11
#define PCM_CFG_TX_WLEN_16   (0x03 << 2)
#define PCM_CFG_FMT_TDM      (0x03 << 0)

// ---- CLK_CTRL 位 ----
#define CLK_CTRL_SCK_XI_BCK  (0x01 << 6)  // PLL 参考源选 BCK
#define CLK_CTRL_SCK_SRC_PLL (1 << 5)     // 系统时钟取自 PLL
#define CLK_CTRL_MST_MODE    (1 << 4)     // 1=主机, 0=从机
#define CLK_CTRL_ADC_SRC_PLL (1 << 3)     // ADC 时钟取自 PLL

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_page = 0xFF;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

void pcm1865_scan(void)
{
    ESP_LOGI(TAG, "I2C 扫描 (0x03..0x77)...");
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  发现设备 @ 0x%02X", addr);
        }
    }
}

esp_err_t pcm1865_init(void)
{
    // 1) 建立 I2C 主机总线
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // 板上若已有上拉可关掉
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    pcm1865_scan();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCM1865_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c dev");

    // 2) 选 Page 0
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PAGE, 0x00), TAG, "page");

    // 3) 输入选择: 默认即 VINL1/VINR1->ADC1, VINL2/VINR2->ADC2 (单端)。
    //    若 4 个麦克风接在其它 VIN 脚, 改写 0x06~0x09 的 [5:0] 选择位。
    //    这里保持默认, 仅显式确保供电通道开启。

    // 4) PGA 增益 = 0 dB (0x00, 0.5dB/step)。需要增益时改 0x01~0x04。
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PGA_VAL_CH1_L, 0x00), TAG, "pga1l");
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PGA_VAL_CH1_R, 0x00), TAG, "pga1r");
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PGA_VAL_CH2_L, 0x00), TAG, "pga2l");
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PGA_VAL_CH2_R, 0x00), TAG, "pga2r");

    // 5) 格式: TDM + TX 字长 16-bit  -> 每通道 16 BCK, 4 槽共 64 BCK = 64*fs
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_PCM_CFG, PCM_CFG_TX_WLEN_16 | PCM_CFG_FMT_TDM),
                        TAG, "pcm_cfg");

    // 6) TDM TX 选择: 0x00 = ADC1+ADC2 全部 4 通道输出到 TDM
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_TDM_TX_SEL, 0x00), TAG, "tdm_sel");

    // 7) TDM 起始偏移: 默认对齐 Philips-TDM (数据滞后 WS 1 BCK)。
    //    若 ESP32 端读到的通道整体错位, 在此调整 (单位 BCK)。
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_TDM_TX_OFFSET, 0x01), TAG, "tdm_off");

    // 8) 时钟: 从机 + PLL 以 BCK 为参考 + ADC/系统时钟取自 PLL
    //    (PCM186x 上电默认即"从机 + 自动时钟检测", 多数情况留默认也能锁;
    //     这里显式写入以确保 BCK-PLL 路径生效)
    ESP_RETURN_ON_ERROR(
        reg_write(PCM186X_CLK_CTRL,
                  CLK_CTRL_SCK_XI_BCK | CLK_CTRL_SCK_SRC_PLL | CLK_CTRL_ADC_SRC_PLL),
        TAG, "clk_ctrl");
    // 若自动时钟检测更稳, 可改为: reg_write(PCM186X_CLK_CTRL, 0x00);

    // 9) 上电运行 (清 PWRDN/SLEEP/STBY)
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_POWER_CTRL, 0x00), TAG, "power");

    (void)s_page;
    ESP_LOGI(TAG, "PCM1865 配置完成: TDM 4ch / 16-bit / slave(BCK-PLL) / fs=%d", SAMPLE_RATE);
    return ESP_OK;
}
