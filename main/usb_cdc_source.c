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
#include <string.h>
#include <stdio.h>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"     // usb_parse_next_descriptor_of_type
#include "usb/cdc_acm_host.h"

#include "rtcm_sink.h"
#include "rtcm_monitor.h"   // valid-RTCM3 gate for the interface sweep

static const char *TAG = "usb_cdc";

// Shared status for the `usb` console command. Plain writes from the driver
// tasks / callbacks; reads are advisory (no lock needed for a status snapshot).
static volatile usb_cdc_status_t s_status;

void usb_cdc_source_status(usb_cdc_status_t *out)
{
    if (out) *out = s_status;
}

// ── Device identity ─────────────────────────────────────────────────────────
// Confirmed on the bench (M0 attach log): Septentrio mosaic-go enumerates under
// the Thesycon VID with three CDC-ACM COM ports + a USB mass-storage interface:
//   itf 0/1 = COM #1 (comm proto 0xFF)   itf 2/3 = COM #2   itf 4/5 = COM #3
//   itf 6   = MSC (internal disk)  — NO ECM/RNDIS network interface present.
// Which COM carries RTCM3 is a receiver-config choice, so we SWEEP the three
// comm-interface indices until one actually streams bytes (see cdc_task).
#define MOSAIC_VID        0x152A
#define MOSAIC_PID        0x85C0

// bInterfaceNumber of each CDC-ACM *communications* interface (the data
// interface is the next one). cdc_acm_host_open()'s interface_idx is this
// bInterfaceNumber (see cdc_host_descriptor_parsing.c: usb_parse_interface_
// descriptor(config, intf_idx, 0, ...)).
static const uint8_t MOSAIC_COM_ITFS[] = {0, 2, 4};
#define MOSAIC_SWEEP_DWELL_MS  6000   // wait this long for bytes before hopping

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
// Walk the active config descriptor's interfaces and record a compact summary
// (bInterfaceNumber:class/subclass per alt-0 interface) into s_status.topo, so
// the composite layout is readable from the periodic line — no need to catch
// the one-shot cdc_acm_host_desc_print(). This is what tells us which itf index
// is the RTCM3-streaming CDC-Data port vs the ECM/notification interfaces.
//
// Reference classes on the Mosaic composite:
//   02/02 = CDC-Comm (ACM control)      0a/00 = CDC-Data (the byte stream)
//   02/06 = CDC-Comm (ECM control)      02/ff = RNDIS-ish vendor control
static void log_topology(usb_device_handle_t usb_dev)
{
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(usb_dev, &cfg) != ESP_OK) {
        return;
    }
    char tmp[USB_TOPO_MAX];
    size_t pos = 0;
    uint8_t n = 0;
    tmp[0] = '\0';

    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(
                    d, cfg->wTotalLength,
                    USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *itf = (const usb_intf_desc_t *)d;
        ESP_LOGI(TAG, "  itf %u alt %u: class=0x%02X sub=0x%02X proto=0x%02X eps=%u",
                 itf->bInterfaceNumber, itf->bAlternateSetting,
                 itf->bInterfaceClass, itf->bInterfaceSubClass,
                 itf->bInterfaceProtocol, itf->bNumEndpoints);
        // Only alt-0 in the compact summary; alt settings just add noise there.
        if (itf->bAlternateSetting == 0 && pos + 12 < sizeof(tmp)) {
            int w = snprintf(tmp + pos, sizeof(tmp) - pos, "%s%u:%02x/%02x",
                             n ? " " : "", itf->bInterfaceNumber,
                             itf->bInterfaceClass, itf->bInterfaceSubClass);
            if (w > 0) {
                pos += (size_t)w;
                n++;
            }
        }
    }
    s_status.num_interfaces = n;
    strlcpy((char *)s_status.topo, tmp, sizeof(s_status.topo));
    ESP_LOGI(TAG, "topology: n=%u if[%s]", n, tmp);
}

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
    log_topology(usb_dev);
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

// Bytes seen since the current interface was opened — the sweep uses this to
// decide whether the bound COM is the one actually streaming RTCM3.
static volatile uint32_t s_rx_since_open;

// The hot path. Runs in the CDC driver task context (NOT an ISR), so a
// non-blocking StreamBuffer send is safe. Return true = we consumed the data.
static bool cdc_data_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    s_rx_since_open += data_len;
    rtcm_sink_push(data, data_len);
    return true;
}

static void cdc_task(void *arg)
{
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    ESP_LOGI(TAG, "cdc_acm_host installed");

    // Register the composite new-device logger (milestone-0 discovery) only AFTER
    // cdc_acm_host_install() has allocated the driver object — the register call
    // dereferences that object, so doing it earlier is a NULL store fault.
    cdc_acm_host_register_new_dev_callback(new_dev_cb);

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,       // we mostly RX; small TX for config cmds
        .in_buffer_size = 2048,       // TODO(ver): field name/existence varies
        .event_cb = cdc_event_cb,
        .data_cb = cdc_data_cb,
        .user_arg = NULL,
    };

    const cdc_acm_line_coding_t lc = {
        .dwDTERate = MOSAIC_BAUD,
        .bCharFormat = 0,   // 1 stop bit
        .bParityType = 0,   // none
        .bDataBits = 8,
    };

    size_t sweep = 0;   // index into MOSAIC_COM_ITFS
    while (1) {
        uint8_t itf = MOSAIC_COM_ITFS[sweep];
        cdc_acm_dev_hdl_t cdc = NULL;
        ESP_LOGI(TAG, "opening Mosaic COM itf=%d (VID=0x%04X PID=0x%04X)...",
                 itf, MOSAIC_VID, MOSAIC_PID);
        esp_err_t err = cdc_acm_host_open(MOSAIC_VID, MOSAIC_PID, itf, &dev_cfg, &cdc);
        if (err != ESP_OK) {
            // Device not present yet (still booting) — retry same itf, don't hop.
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Mosaic CDC opened on itf=%d", itf);
        s_status.cdc_open = true;
        s_status.cur_itf = itf;
        s_rx_since_open = 0;

        // Best-effort: some virtual COMs reject SET_LINE_CODING; ignore errors.
        cdc_acm_host_line_coding_set(cdc, &lc);
        cdc_acm_host_set_control_line_state(cdc, /*dtr=*/true, /*rts=*/true);

        // Dwell: latch on VALID RTCM3, not just any bytes. One Mosaic COM streams
        // NMEA @1Hz (confirmed on the bench), which would otherwise hijack the
        // sweep. Watch the monitor's CRC-valid frame count for an advance; raw
        // bytes alone (NMEA/echo) don't qualify. If nothing valid arrives, hop.
        rtcm_mon_stats_t base;
        rtcm_monitor_get(&base);

        bool disconnected = false;
        bool streaming = false;
        for (int waited = 0; waited < MOSAIC_SWEEP_DWELL_MS; waited += 250) {
            if (xSemaphoreTake(s_disconnected, pdMS_TO_TICKS(250)) == pdTRUE) {
                disconnected = true;   // handle already closed by cdc_event_cb
                break;
            }
            rtcm_mon_stats_t now;
            rtcm_monitor_get(&now);
            if (now.valid_frames > base.valid_frames) { streaming = true; break; }
        }

        if (disconnected) {
            ESP_LOGW(TAG, "Mosaic disconnected (itf=%d), rescanning from itf 0", itf);
            s_status.cur_itf = 0xFF;
            sweep = 0;
            continue;
        }
        if (streaming) {
            s_status.stream_itf = itf;
            ESP_LOGI(TAG, "*** valid RTCM3 on itf=%d (%lu raw B seen) — latched ***",
                     itf, (unsigned long)s_rx_since_open);
            xSemaphoreTake(s_disconnected, portMAX_DELAY);   // stay until unplug
            ESP_LOGW(TAG, "stream itf=%d disconnected, rescanning", itf);
            s_status.cur_itf = 0xFF;
            sweep = 0;
            continue;
        }

        // No valid RTCM3 in the dwell window. The raw-byte count distinguishes a
        // truly silent COM (0 B) from non-RTCM3 chatter like NMEA (>0 B). Hop.
        ESP_LOGW(TAG, "itf=%d no RTCM3 in %d ms (%lu raw B) — hopping",
                 itf, MOSAIC_SWEEP_DWELL_MS, (unsigned long)s_rx_since_open);
        cdc_acm_host_close(cdc);
        s_status.cdc_open = false;
        s_status.cur_itf = 0xFF;
        sweep = (sweep + 1) % (sizeof(MOSAIC_COM_ITFS) / sizeof(MOSAIC_COM_ITFS[0]));
    }
}

esp_err_t usb_cdc_source_start(void)
{
    s_status.cur_itf = 0xFF;
    s_status.stream_itf = 0xFF;

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

    // new_dev_cb is registered inside cdc_task, right after cdc_acm_host_install()
    // — it must not run before the driver object exists.
    if (xTaskCreate(cdc_task, "cdc", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
