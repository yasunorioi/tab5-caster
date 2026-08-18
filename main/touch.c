// touch.c — see touch.h.
//
// The ST7123 register addresses / touch-data bit layout below follow the ESPHome
// ST7123 touchscreen component (GPLv3) as a *register-map reference only* —
// these are hardware facts dictated by the silicon; no ESPHome code was copied.
// This file is original work, not a GPL derivative. See NOTICE.

#include "touch.h"
#include "board_power.h"   // board_i2c_bus()

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "touch";

// ST7123 integrated touch (this panel rev). 16-bit register addresses.
#define ST7123_ADDR          0x55
#define REG_ADV_TOUCH_INFO   0x0010   // block start
#define REG_TOUCH_DATA       0x0014   // first touch point
#define TOUCH_STRIDE         7        // bytes per point
#define TOUCH_VALID          0x80     // point[0] bit7
#define COORD_HIGH_MASK      0x3F
// Read the header (0x10..0x13) + one point (7 B). Point 0 lands at offset 4.
#define READ_LEN             ((REG_TOUCH_DATA - REG_ADV_TOUCH_INFO) + TOUCH_STRIDE)
#define POINT0_OFF           (REG_TOUCH_DATA - REG_ADV_TOUCH_INFO)

#define POLL_MS              100      // 10 Hz — catches taps, cheap (11 B read)
#define I2C_TIMEOUT_MS       50
// Phantom rejection without hurting wake latency: count valid samples in a
// short sliding window rather than requiring N in a row. The ST7123's phantom
// is a LONE valid sample, so it never reaches 2-in-window; a real tap (even if
// the controller flickers valid/invalid mid-touch) easily does. Strict
// consecutive-N was too harsh — quick wake taps didn't register.
#define TOUCH_WIN            5        // window of polls (~500 ms), <= 8
#define TOUCH_MIN            2        // >= this many valid in the window = real

static i2c_master_dev_handle_t s_dev;
static volatile int64_t        s_last_us;
static volatile bool           s_present;

// Read READ_LEN bytes starting at REG_ADV_TOUCH_INFO. Returns ESP_OK on a good
// I2C transaction (whether or not a finger is down).
static esp_err_t read_block(uint8_t *buf)
{
    uint8_t reg[2] = { (REG_ADV_TOUCH_INFO >> 8) & 0xFF, REG_ADV_TOUCH_INFO & 0xFF };
    return i2c_master_transmit_receive(s_dev, reg, sizeof(reg), buf, READ_LEN,
                                       I2C_TIMEOUT_MS);
}

esp_err_t touch_read_point(bool *valid, int *x, int *y)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t b[READ_LEN];
    esp_err_t err = read_block(b);
    if (err != ESP_OK) return err;
    const uint8_t *p = b + POINT0_OFF;
    bool v = (p[0] & TOUCH_VALID) != 0;
    if (valid) *valid = v;
    if (x) *x = ((p[0] & COORD_HIGH_MASK) << 8) | p[1];
    if (y) *y = ((p[2] & COORD_HIGH_MASK) << 8) | p[3];
    return ESP_OK;
}

static void poll_task(void *arg)
{
    (void)arg;
    uint8_t b[READ_LEN];
    uint8_t hist = 0;   // bit i = validity of the poll i cycles ago (bit0 = now)
    while (1) {
        int valid = 0;
        if (read_block(b) == ESP_OK)
            valid = (b[POINT0_OFF] & TOUCH_VALID) ? 1 : 0;
        hist = (uint8_t)((hist << 1) | valid);
        // Real touch only if >= TOUCH_MIN valid in the last TOUCH_WIN polls; a
        // lone phantom sample can never reach it, so it won't reset the timer.
        if (__builtin_popcount(hist & ((1u << TOUCH_WIN) - 1)) >= TOUCH_MIN)
            s_last_us = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool    touch_present(void)          { return s_present; }
int64_t touch_last_activity_us(void) { return s_last_us; }

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "board I2C bus not up — touch disabled");
        return ESP_ERR_INVALID_STATE;
    }
    if (i2c_master_probe(bus, ST7123_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "no touch @ 0x%02X (GT911 rev?) — idle-off disabled",
                 ST7123_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ST7123_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add 0x%02X: %s", ST7123_ADDR, esp_err_to_name(err));
        return err;
    }

    s_last_us = esp_timer_get_time();   // seed: age idle time from boot
    s_present = true;
    if (xTaskCreate(poll_task, "touch", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        s_present = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ST7123 touch up @ 0x%02X (%d Hz activity poll)",
             ST7123_ADDR, 1000 / POLL_MS);
    return ESP_OK;
}
