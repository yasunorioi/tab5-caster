// display.c — see display.h.

#include "display.h"
#include "board_power.h"
#include "tab5_ili9881c_init.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_st7123.h"
#include "tab5_st7123_init.h"
#include "esp_cache.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"

static const char *TAG = "display";

// ── panel electrical spec (verified from M5Tab5-UserDemo BSP) ────────────────
// Two hardware revisions share the same LDO/backlight/reset but need different
// DSI timing + init tables + panel drivers:
//   - ILI9881C (pre-2025-10 units, paired with a GT911 touch @ 0x14/0x5D)
//   - ST7123   (newer units, integrated touch @ 0x55)
// display_init() I2C-probes 0x55 to pick the path (see detect_panel).
#define MIPI_LDO_CHAN        3       // LDO_VO3 -> VDD_MIPI_DPHY
#define MIPI_LDO_MV          2500
#define MIPI_LANES           2

// ILI9881C timing ("720x1280 RGB24 60Hz").
#define ILI_LANE_MBPS        730
#define ILI_DPI_CLK_MHZ      60
#define ILI_HSYNC_BP  140
#define ILI_HSYNC_PW  40
#define ILI_HSYNC_FP  40
#define ILI_VSYNC_BP  20
#define ILI_VSYNC_PW  4
#define ILI_VSYNC_FP  20

// ST7123 timing (faster lane + clock, different porches).
#define ST_LANE_MBPS         965
#define ST_DPI_CLK_MHZ       70
#define ST_HSYNC_BP   40
#define ST_HSYNC_PW   2
#define ST_HSYNC_FP   40
#define ST_VSYNC_BP   8
#define ST_VSYNC_PW   2
#define ST_VSYNC_FP   220

// The newer-revision ST7123 integrated touch controller acks here; the older
// ILI9881C revision instead has a GT911 touch at 0x14/0x5D.
#define ST712X_TOUCH_ADDR    0x55

// Backlight: LEDC on a direct P4 GPIO (not behind the expander).
#define BL_GPIO              22
#define BL_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define BL_LEDC_CH           LEDC_CHANNEL_1
#define BL_LEDC_TIMER        LEDC_TIMER_0
#define BL_LEDC_FREQ_HZ      5000
#define BL_LEDC_RES          LEDC_TIMER_12_BIT
#define BL_DUTY_MAX          4095

// ── LCD-reset I/O expander: PI4IOE #1 @ 0x43 (P4 = LCD_RST) ───────────────────
// Same PI4IOE register map as board_power's expander #2 (NOT PCA9555).
#define PI4IOE1_ADDR         0x43
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
// Port map: P1=SPK_EN P2=EXT5V_EN P4=LCD_RST P5=TP_RST P6=CAM_RST.
#define OUT_ALL_RELEASED     0b01110110  // P1,P2,P4,P5,P6 high (LCD_RST high)
#define OUT_LCD_IN_RESET     0b01100110  // same but P4 low (LCD_RST asserted)
#define LCD_RST_BIT          4
#define I2C_TIMEOUT_MS       50

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static i2c_master_dev_handle_t   s_pi4ioe1;

esp_lcd_panel_handle_t    display_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t display_io(void)    { return s_io; }

static esp_err_t exp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pi4ioe1, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// Init PI4IOE #1 and pulse LCD_RST (P4) low->high to hard-reset the panel.
static esp_err_t lcd_reset_expander_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "board I2C bus not up (call board_power_init first)");
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE1_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_pi4ioe1),
                        TAG, "add pi4ioe1 (0x%02X)", PI4IOE1_ADDR);

    // Same init order as board_power: reset, directions, clear high-Z, pulls.
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_CHIP_RESET, 0xFF), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_IO_DIR,   0b01111111), TAG, "dir");
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_OUT_H_IM, 0x00),       TAG, "hiz");
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_PULL_SEL, 0b01111111), TAG, "psel");
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_PULL_EN,  0b01111111), TAG, "pen");

    // Reset pulse: assert LCD_RST low, hold, release high, wait for panel wake.
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_OUT_SET, OUT_LCD_IN_RESET), TAG, "rst-lo");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(exp_write(PI4IO_REG_OUT_SET, OUT_ALL_RELEASED), TAG, "rst-hi");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "PI4IOE #1 up, LCD_RST released (P%d)", LCD_RST_BIT);
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
    const ledc_channel_config_t c = {
        .gpio_num   = BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CH,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,           // start dark; caller raises after first draw
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&c), TAG, "ledc ch");
    return ESP_OK;
}

void display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)BL_DUTY_MAX * percent / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CH, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CH);
}

// Which panel this board has. The two revisions need different DSI timing and
// panel drivers; probing the touch controller tells them apart.
typedef enum { PANEL_ILI9881C, PANEL_ST7123 } panel_kind_t;

static panel_kind_t detect_panel(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus && i2c_master_probe(bus, ST712X_TOUCH_ADDR, 50) == ESP_OK) {
        ESP_LOGI(TAG, "touch @ 0x%02X acked -> ST7123 panel", ST712X_TOUCH_ADDR);
        return PANEL_ST7123;
    }
    ESP_LOGI(TAG, "no 0x%02X ack -> assuming ILI9881C panel", ST712X_TOUCH_ADDR);
    return PANEL_ILI9881C;
}

esp_err_t display_init(void)
{
    if (s_panel) return ESP_OK;   // idempotent

    ESP_RETURN_ON_ERROR(lcd_reset_expander_init(), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    const panel_kind_t kind = detect_panel();
    const bool st = (kind == PANEL_ST7123);

    // 1) Power the MIPI D-PHY (must precede the DSI bus). Same on both revisions.
    esp_ldo_channel_handle_t phy_ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_LDO_CHAN, .voltage_mv = MIPI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo), TAG, "ldo");

    // 2) DSI bus — lane rate differs per panel.
    esp_lcd_dsi_bus_handle_t dsi = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = MIPI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = st ? ST_LANE_MBPS : ILI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "dsi bus");

    // 3) DBI command channel (8/8 bits). Same on both.
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &s_io), TAG, "dbi io");

    // 4) DPI (RGB565) — clock + porches differ per panel.
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = st ? ST_DPI_CLK_MHZ : ILI_DPI_CLK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size = TAB5_LCD_H_RES, .v_size = TAB5_LCD_V_RES,
            .hsync_back_porch  = st ? ST_HSYNC_BP : ILI_HSYNC_BP,
            .hsync_pulse_width = st ? ST_HSYNC_PW : ILI_HSYNC_PW,
            .hsync_front_porch = st ? ST_HSYNC_FP : ILI_HSYNC_FP,
            .vsync_back_porch  = st ? ST_VSYNC_BP : ILI_VSYNC_BP,
            .vsync_pulse_width = st ? ST_VSYNC_PW : ILI_VSYNC_PW,
            .vsync_front_porch = st ? ST_VSYNC_FP : ILI_VSYNC_FP,
        },
        .flags.use_dma2d = true,
    };

    // 5) Build the panel with its vendor init table. bits_per_pixel is cosmetic
    // (COLMOD isn't sent by these drivers); the framebuffer stays RGB565. M5 uses
    // 24 on the ST7123 path and 16 on ILI9881C — match each.
    if (st) {
        st7123_vendor_config_t vendor = {
            .init_cmds      = tab5_st7123_init,
            .init_cmds_size = tab5_st7123_init_len,
            .mipi_config = { .dsi_bus = dsi, .dpi_config = &dpi_cfg, .lane_num = MIPI_LANES },
        };
        const esp_lcd_panel_dev_config_t dev_cfg = {
            .reset_gpio_num = -1,          // reset is on the expander, done above
            .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 24,
            .vendor_config  = &vendor,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7123(s_io, &dev_cfg, &s_panel), TAG, "st7123");
    } else {
        ili9881c_vendor_config_t vendor = {
            .init_cmds      = tab5_ili9881c_init,
            .init_cmds_size = tab5_ili9881c_init_len,
            .mipi_config = { .dsi_bus = dsi, .dpi_config = &dpi_cfg, .lane_num = MIPI_LANES },
        };
        const esp_lcd_panel_dev_config_t dev_cfg = {
            .reset_gpio_num = -1,
            .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config  = &vendor,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9881c(s_io, &dev_cfg, &s_panel), TAG, "ili9881c");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    ESP_LOGI(TAG, "%s up: %dx%d, %d lanes @ %d Mbps, DPI %d MHz RGB565",
             st ? "ST7123" : "ILI9881C", TAB5_LCD_H_RES, TAB5_LCD_V_RES, MIPI_LANES,
             st ? ST_LANE_MBPS : ILI_LANE_MBPS, st ? ST_DPI_CLK_MHZ : ILI_DPI_CLK_MHZ);
    return ESP_OK;
}

void display_fill(uint16_t color565)
{
    if (!s_panel) { ESP_LOGW(TAG, "fill before init"); return; }
    // Write the scanout framebuffer directly. draw_bitmap on a 1-FB DPI panel is
    // async (DMA2D), so a tight band loop trips "previous draw not finished";
    // the FB is the live buffer, so filling it in place just shows immediately.
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || fb == NULL) {
        ESP_LOGW(TAG, "get_frame_buffer failed");
        return;
    }
    uint16_t *px = (uint16_t *)fb;
    size_t npx = (size_t)TAB5_LCD_H_RES * TAB5_LCD_V_RES;
    for (size_t i = 0; i < npx; i++) px[i] = color565;
    // The FB lives in cacheable PSRAM; the DPI DMA scans PSRAM directly, so a CPU
    // write must be flushed back or the panel keeps showing the stale (black) FB.
    esp_cache_msync(fb, npx * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}
