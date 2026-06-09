// ============================================================================
//  PCM1865 配置 (I2C)  —— TDM 4 通道, 从机模式, 时钟由 BCK 经内部 PLL 派生
//
//  寄存器地址/位域取自 TI PCM186x 数据手册 (SLAS831) 与 Linux 内核驱动
//  sound/soc/codecs/pcm186x.h。所有写值含义见注释。
//  ⚠ 现场 bring-up 必须用逻辑分析仪核对 DOUT 时序, 不对则按注释调 OFFSET/极性。
// ============================================================================
#include "pcm1865.h"
#include "config.h"
#include "i2s_capture.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#define PCM186X_CLK_CTRL          0x20   // 主从 / PLL 源 / SCK 源 / 时钟检测
#define PCM186X_PLL_CTRL          0x28   // bit4=LOCK(只读) bit1=REF_SEL bit0=EN
#define PCM186X_POWER_CTRL        0x70
// ---- 只读状态寄存器 (bring-up 诊断) ----
#define PCM186X_DEVICE_STATUS     0x72
#define PCM186X_FSAMPLE_STATUS    0x73
#define PCM186X_DIV_STATUS        0x74
#define PCM186X_CLK_STATUS        0x75   // bit6 LRCKHLT,5 BCKHLT,4 SCKHLT,2 LRCKERR,1 BCKERR,0 SCKERR
#define PCM186X_SUPPLY_STATUS     0x78   // bit2 DVDD,1 AVDD,0 LDO

// ---- PCM_CFG 位 ----
// RX_WLEN[7:6], TX_WLEN[3:2], FMT[1:0]; 16-bit=0b11, TDM=0b11
#define PCM_CFG_TX_WLEN_16   (0x03 << 2)
#define PCM_CFG_FMT_TDM      (0x03 << 0)

// ---- CLK_CTRL 位 ----
#define CLK_CTRL_SCK_XI_BCK  (0x01 << 6)  // PLL 参考源选 BCK
#define CLK_CTRL_SCK_SRC_PLL (1 << 5)     // 系统时钟取自 PLL
#define CLK_CTRL_MST_MODE    (1 << 4)     // 1=主机, 0=从机
#define CLK_CTRL_ADC_SRC_PLL (1 << 3)     // ADC 时钟取自 PLL

// ---- BCK-PLL(Path B) 系数表: 按 SAMPLE_RATE 取自数据手册 SLAS831D Table 13 ----
//   BCK=64fs 作 PLL 参考, VCO 恒 98.304MHz, 分频得 DSP1=512fs / DSP2=256fs / ADC=128fs。
#if   SAMPLE_RATE == 16000
  #define PLLB_J      48              // VCO = 1.024M x R(2) x 48 / P(1) = 98.304MHz
  #define PLLB_DIV_DSP1 12
  #define PLLB_DIV_DSP2 24
  #define PLLB_DIV_ADC  48
#elif SAMPLE_RATE == 48000
  #define PLLB_J      16              // VCO = 3.072M x R(2) x 16 / P(1) = 98.304MHz
  #define PLLB_DIV_DSP1 4
  #define PLLB_DIV_DSP2 8
  #define PLLB_DIV_ADC  16
#else
  #error "Path B PLL 系数仅含 16000/48000 Hz; 其它采样率请按数据手册 Table 13 补全本表"
#endif

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_page = 0xFF;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 1000);
}

// 轮询 PLL LOCK 位 (0x28 bit4) 最多 ~400ms
static bool pll_wait_lock(void)
{
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t s = 0;
        if (reg_read(PCM186X_PLL_CTRL, &s) == ESP_OK && (s & 0x10)) return true;
    }
    return false;
}

// bring-up 诊断: 回读关键状态寄存器, 判断时钟/PLL/供电是否正常
void pcm1865_dump_status(void)
{
    uint8_t clk_ctrl = 0, pll = 0, dev = 0, fs = 0, divs = 0, clk = 0, sup = 0;
    reg_write(PCM186X_PAGE, 0x00);
    reg_read(PCM186X_CLK_CTRL,      &clk_ctrl);
    reg_read(PCM186X_PLL_CTRL,      &pll);
    reg_read(PCM186X_DEVICE_STATUS, &dev);
    reg_read(PCM186X_FSAMPLE_STATUS,&fs);
    reg_read(PCM186X_DIV_STATUS,    &divs);
    reg_read(PCM186X_CLK_STATUS,    &clk);
    reg_read(PCM186X_SUPPLY_STATUS, &sup);

    ESP_LOGI(TAG, "=== PCM1865 状态 ===");
    ESP_LOGI(TAG, "CLK_CTRL(0x20)=0x%02X  PLL_CTRL(0x28)=0x%02X (LOCK=%d EN=%d REF_SEL=%d)",
             clk_ctrl, pll, !!(pll & 0x10), !!(pll & 0x01), !!(pll & 0x02));
    ESP_LOGI(TAG, "DEVICE_STATUS(0x72)=0x%02X  FSAMPLE(0x73)=0x%02X  DIV(0x74)=0x%02X",
             dev, fs, divs);
    ESP_LOGI(TAG, "CLK_STATUS(0x75)=0x%02X  LRCKHLT=%d BCKHLT=%d SCKHLT=%d | LRCKERR=%d BCKERR=%d SCKERR=%d",
             clk, !!(clk&0x40), !!(clk&0x20), !!(clk&0x10), !!(clk&0x04), !!(clk&0x02), !!(clk&0x01));
    ESP_LOGI(TAG, "SUPPLY(0x78)=0x%02X  DVDD=%d AVDD=%d LDO=%d",
             sup, !!(sup&0x04), !!(sup&0x02), !!(sup&0x01));
    ESP_LOGI(TAG, "判读: SCKHLT=1 => 无系统时钟(缺 MCLK 或 PLL 没锁); BCKHLT/LRCKHLT=1 => ESP32 没送 BCK/LRCK");
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

    // 8a) 先上电 (清 PWRDN/SLEEP/STBY) —— 必须在配 PLL 之前!
    //     数据手册 Table 19: PLL 在 STANDBY 下是 OFF 的, 若先配 PLL 后上电, 轮询时 PLL 没通电永远锁不上。
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_POWER_CTRL, 0x00), TAG, "power");
    vTaskDelay(pdMS_TO_TICKS(10));

    // 8b) 时钟配置 —— 这是"全零静音"的关键
#if USE_MCLK_OUTPUT
    // Path A: ESP32 送 MCLK 到 SCK 脚 -> 从机 + 自动时钟检测 (CLKDET_EN=1),
    //         系统/ADC/DSP 时钟全部取自 SCK 脚, 自动识别 SCK/fs 比率。最稳。
    ESP_RETURN_ON_ERROR(reg_write(PCM186X_CLK_CTRL, 0x01 /*CLKDET_EN*/), TAG, "clk_ctrl");
#else
    // Path B: ADC slave PLL 模式 (无 MCLK, BCK=64fs 作 PLL 参考)。
    // 数据手册 SLAS831D Table 13 (PCM1865 4ch BCK PLL Settings), BCK=64fs 作参考。
    //   系数随 SAMPLE_RATE 取自上方 PLLB_* 表; P=1, R=2, D=0, VCO=98.304MHz(落在 64~100MHz)。
    //   分频(源均为 PLL): DSP1=512fs, DSP2=256fs, ADC=128fs。
    // 手动配置时关闭自动时钟检测(CLKDET_EN=0), 否则会覆盖手动系数。
    // 寄存器编码: R 存 R-1; DSP/ADC 分频存 N-1 (同内核驱动 BCK_DIV 惯例); J 直存;
    //            P 编码手册措辞不明 -> P 直存与 P-1 两种都试, 谁锁上用谁。
    bool locked = false;

    // ---- 尝试 1: 自动 (数据手册: BCK-PLL 从机 + 标准采样率 = 自动配置) ----
    // 先填好分频(自动锁后 ADC/DSP 需要), 开 CLKDET_EN + 源=PLL, REF_SEL=BCK, 让状态机进 BCK PLL 模式。
    reg_write(0x2B /*J*/, PLLB_J); reg_write(0x2C,0); reg_write(0x2D,0);
    reg_write(0x21 /*DSP1*/, PLLB_DIV_DSP1 - 1);
    reg_write(0x22 /*DSP2*/, PLLB_DIV_DSP2 - 1);
    reg_write(0x23 /*ADC*/,  PLLB_DIV_ADC  - 1);
    reg_write(PCM186X_CLK_CTRL, 0x0F);                 // CLKDET_EN=1 + 源=PLL
    reg_write(PCM186X_PLL_CTRL, 0x03);                 // REF_SEL=BCK | EN
    // 关键: 抖动 BCK/LRCK(停>3拍再恢复)触发从机自动重检测, 否则连续时钟下检测器不重配。
    for (int t = 0; t < 3 && !locked; t++) {
        i2s_capture_restart();
        locked = pll_wait_lock();
        ESP_LOGI(TAG, "PLL 自动+时钟抖动#%d -> %s", t, locked ? "锁定" : "未锁");
    }

    // ---- 尝试 2: 手动, 把 R/P/REF_SEL 编码全组合扫一遍 ----
    // 只有 P/R/D/REF 影响锁定(J=16 已确认, 分频不影响); R 直存 vs R-1 是上次没试的关键变量。
    if (!locked) {
        reg_write(PCM186X_CLK_CTRL, 0x0E);             // 关 CLKDET, 手动
        const uint8_t rvals[2] = { 2, 1 };             // R 直存(2) / R-1(1)
        const uint8_t pvals[2] = { 1, 0 };             // P 直存(1) / P-1(0)
        const uint8_t refs[2]  = { 0x02, 0x00 };       // REF_SEL bit1: 1 / 0
        for (int ri = 0; ri < 2 && !locked; ri++)
        for (int pi = 0; pi < 2 && !locked; pi++)
        for (int fi = 0; fi < 2 && !locked; fi++) {
            reg_write(PCM186X_PLL_CTRL, 0x00);
            reg_write(0x29 /*P*/, pvals[pi]);
            reg_write(0x2A /*R*/, rvals[ri]);
            reg_write(PCM186X_PLL_CTRL, refs[fi] | 0x01);
            i2s_capture_restart();                     // 时钟抖动, 触发离开 Clock Waiting
            locked = pll_wait_lock();
            ESP_LOGI(TAG, "PLL 手动 R=%d P=%d REF=%d -> %s",
                     rvals[ri], pvals[pi], !!(refs[fi] & 0x02), locked ? "锁定" : "未锁");
        }
    }
    ESP_LOGI(TAG, "PLL 锁定结果: %s", locked ? "OK" : "全部失败(疑 BCK 信号质量, 示波器量 PCM1865 BCK 脚)");
#endif

    (void)s_page;
    ESP_LOGI(TAG, "PCM1865 配置完成: TDM 4ch / 16-bit / slave(BCK-PLL) / fs=%d", SAMPLE_RATE);
    return ESP_OK;
}
