// board_power.c — see board_power.h.
//
// Register map + init values are lifted verbatim from the official M5Stack
// Tab5 BSP (M5Tab5-UserDemo .../m5stack_tab5.c, bsp_io_expander_pi4ioe_init +
// bsp_set_usb_5v_en). The expander is a PI4IOE5V6408-style 8-bit part whose
// register map is NOT the PCA9555/TCA6416 one — direction is 0x03, output is
// 0x05, and there is a High-Impedance register (0x07) that MUST be cleared or
// an "output" pin still floats. USB5V_EN = P3 (bit 3) on expander #2 (0x44).

#include "board_power.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "board_pwr";

// Tab5 system I2C bus (BSP_I2C_*).
#define TAB5_I2C_PORT       0
#define TAB5_I2C_SDA        GPIO_NUM_31
#define TAB5_I2C_SCL        GPIO_NUM_32
#define TAB5_I2C_HZ         400000

// PI4IOE #2 carries USB5V_EN (P3) and WLAN_PWR_EN (P0). addr pin high = 0x44.
#define PI4IOE2_ADDR        0x44

// PI4IOE register map (NOT PCA9555 layout).
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07   // 1 = high-impedance, 0 = actively driven
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_INT_MASK   0x11

#define USB5V_EN_BIT         3      // P3 on expander #2
#define I2C_TIMEOUT_MS       50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pi4ioe2;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pi4ioe2, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t board_usb_5v_en(bool en)
{
    if (!s_pi4ioe2) return ESP_ERR_INVALID_STATE;

    // Read-modify-write so we don't clobber P0 (WLAN_PWR_EN).
    uint8_t reg = PI4IO_REG_OUT_SET, cur = 0;
    esp_err_t err = i2c_master_transmit_receive(s_pi4ioe2, &reg, 1, &cur, 1,
                                                I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    if (en) cur |=  (1u << USB5V_EN_BIT);
    else    cur &= ~(1u << USB5V_EN_BIT);
    err = reg_write(PI4IO_REG_OUT_SET, cur);
    if (err == ESP_OK) ESP_LOGI(TAG, "USB-A 5V %s", en ? "ON" : "OFF");
    return err;
}

esp_err_t board_power_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = TAB5_I2C_PORT,
        .sda_io_num                   = TAB5_I2C_SDA,
        .scl_io_num                   = TAB5_I2C_SCL,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE2_ADDR,
        .scl_speed_hz    = TAB5_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pi4ioe2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add pi4ioe2 (0x%02X): %s", PI4IOE2_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    // Init sequence (verbatim from M5 BSP). Order matters: reset, set
    // directions, DISABLE high-Z on the driven pins, configure pulls, then
    // drive P0(WLAN_PWR_EN)+P3(USB5V_EN) high.
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_CHIP_RESET, 0xFF));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_IO_DIR,     0b10111001));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_OUT_H_IM,   0b00000110));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_PULL_SEL,   0b10111001));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_PULL_EN,    0b11111001));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_IN_DEF_STA, 0b01000000));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_INT_MASK,   0b10111111));
    ESP_ERROR_CHECK(reg_write(PI4IO_REG_OUT_SET,    0b00001001));

    ESP_LOGI(TAG, "PI4IOE #2 up, USB-A 5V enabled (P3)");
    return ESP_OK;
}
