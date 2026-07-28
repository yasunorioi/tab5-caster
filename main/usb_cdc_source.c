// usb_cdc_source.c — USB Host CDC-ACM driver skeleton. See usb_cdc_source.h.
//
// Compiles clean for esp32p4 on ESP-IDF 5.4.4 with usb_host_cdc_acm 2.4.0.
// Runtime is UNTESTED — no hardware yet. Marked TODO(hw) where a real device is
// needed (VID/PID/interface, HS-OTG port + VBUS).
//
// Data flow:
//   USB HS OTG  ->  cdc_acm_host data_cb  ->  rtcm_sink_push()  ->  StreamBuffer
//
// Bring-up order (see README milestones):
//   M0: device attaches, new_dev_cb logs VID/PID + every interface descriptor.
//       Fill in MOSAIC_VID / MOSAIC_PID / MOSAIC_CDC_ITF from that log.
//   M1: data_cb fires; hex dump shows RTCM3 preamble 0xD3 (rtcm_sink consumer).

#include "usb_cdc_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#include "rtcm_sink.h"

static const char *TAG = "usb_cdc";

// Shared status for the `usb` console command. Plain writes from the driver
// tasks / callbacks; reads are advisory (no lock needed for a status snapshot).
static volatile usb_cdc_status_t s_status;

void usb_cdc_source_status(usb_cdc_status_t *out)
{
    if (out) *out = s_status;
}

// ── Device identity ─────────────────────────────────────────────────────────
// TODO(hw): read these from the M0 attach log (new_dev_cb below) and pin them.
// Septentrio mosaic USB VID/PID and which CDC interface index carries the
// RTCM3 stream must be confirmed on the bench. 0x0000 = "match any" placeholder.
#define MOSAIC_VID        0x0000
#define MOSAIC_PID        0x0000
#define MOSAIC_CDC_ITF    0        // composite: the RTCM-streaming COM interface

// USB CDC ignores the real line rate, but the Mosaic virtual COM may honor it.
// Set the Mosaic COM to match (RTCM3 MSM7 all-constellation @1Hz wants margin).
#define MOSAIC_BAUD       460800

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "usb_host installed");
    s_status.host_installed = true;
    xTaskNotifyGive((TaskHandle_t)arg);   // tell starter the host stack is up

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "no more clients, freeing devices");
            usb_host_device_free_all();
        }
    }
}

// Fired by the CDC driver when a new CDC-ACM device shows up. Milestone-0: log
// everything so we can fill in VID/PID/interface. `cdc_acm_host_desc_print`
// dumps the full descriptor tree (composite interfaces included).
static void new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) == ESP_OK) {
        ESP_LOGI(TAG, "CDC device attached: VID=0x%04X PID=0x%04X",
                 desc->idVendor, desc->idProduct);
        s_status.device_attached = true;
        s_status.vid = desc->idVendor;
        s_status.pid = desc->idProduct;
    }
    // TODO(hw): also call usb_host_get_active_config_descriptor() and walk the
    // interfaces to identify which CDC-ACM interface streams RTCM3.
}

static SemaphoreHandle_t s_disconnected;

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Mosaic disconnected");
        // Close on the driver's terms and let the open loop retry.
        cdc_acm_host_close(event->data.cdc_hdl);
        s_status.cdc_open = false;
        xSemaphoreGive(s_disconnected);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "serial state 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

// The hot path. Runs in the CDC driver task context (NOT an ISR), so a
// non-blocking StreamBuffer send is safe. Return true = we consumed the data.
static bool cdc_data_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    rtcm_sink_push(data, data_len);
    return true;
}

static void cdc_task(void *arg)
{
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    ESP_LOGI(TAG, "cdc_acm_host installed");

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,       // we mostly RX; small TX for config cmds
        .in_buffer_size = 2048,       // TODO(ver): field name/existence varies
        .event_cb = cdc_event_cb,
        .data_cb = cdc_data_cb,
        .user_arg = NULL,
    };

    while (1) {
        cdc_acm_dev_hdl_t cdc = NULL;
        ESP_LOGI(TAG, "waiting for Mosaic (VID=0x%04X PID=0x%04X itf=%d)...",
                 MOSAIC_VID, MOSAIC_PID, MOSAIC_CDC_ITF);
        esp_err_t err = cdc_acm_host_open(MOSAIC_VID, MOSAIC_PID, MOSAIC_CDC_ITF,
                                          &dev_cfg, &cdc);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Mosaic CDC opened");
        s_status.cdc_open = true;
        cdc_acm_host_desc_print(cdc);    // M0: full descriptor dump

        const cdc_acm_line_coding_t lc = {
            .dwDTERate = MOSAIC_BAUD,
            .bCharFormat = 0,   // 1 stop bit
            .bParityType = 0,   // none
            .bDataBits = 8,
        };
        // Best-effort: some virtual COMs reject SET_LINE_CODING; ignore errors.
        cdc_acm_host_line_coding_set(cdc, &lc);
        cdc_acm_host_set_control_line_state(cdc, /*dtr=*/true, /*rts=*/true);

        // Block until the device disconnects; data flows via cdc_data_cb.
        xSemaphoreTake(s_disconnected, portMAX_DELAY);
        ESP_LOGW(TAG, "reopening after disconnect");
    }
}

esp_err_t usb_cdc_source_start(void)
{
    s_disconnected = xSemaphoreCreateBinary();
    if (s_disconnected == NULL) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    // Host lib task: pinned, generous stack (USB host is stack-hungry).
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, self, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    // Wait until usb_host_install() completed before installing the class driver.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Register the composite new-device logger (milestone-0 discovery).
    cdc_acm_host_register_new_dev_callback(new_dev_cb);

    if (xTaskCreate(cdc_task, "cdc", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
