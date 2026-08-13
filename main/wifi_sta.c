// wifi_sta.c — see wifi_sta.h. WiFi STA over the C6 (ESP-Hosted).
//
// Credentials live in NVS, not the firmware. Set them at runtime with the
// `wifiset <ssid> <pass>` console command (persists + reboots); the box then
// joins STA on every boot and starts the caster on GOT_IP.
//
// NOTE: a SoftAP browser/field-provisioning portal was attempted but SoftAP /
// APSTA over ESP-Hosted on the C6 is a known-broken upstream area (the AP never
// beacons: no WIFI_EVENT_AP_START, esp_wifi_get_mac -1). STA works fine, so we
// provision over the USB console for now; revisit field provisioning (BLE) once
// esp-hosted AP support is fixed.

#include "wifi_sta.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"        // provided by esp_wifi_remote (radio runs on the C6)
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "caster.h"
#include "upstream.h"

static const char *TAG = "wifi";

#define NVS_NS "wifi"

static bool s_caster_started;

// ── credential storage (NVS) ─────────────────────────────────────────────────

static bool creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

void wifi_set_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass ? pass : ""));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "saved credentials for SSID '%s' — rebooting to connect", ssid);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "credentials erased — rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

// ── events ───────────────────────────────────────────────────────────────────

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR " — caster reachable on :2101 (rtk.local)",
                 IP2STR(&e->ip_info.ip));
        if (!s_caster_started) {
            s_caster_started = true;
            caster_start();
            // Start pushing the base RTCM3 to the cloud caster (idles until
            // provisioned via `upstreamset`). Independent of the local caster.
            upstream_start();
        }
    }
}

// ── entry ────────────────────────────────────────────────────────────────────

static void nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

// SOFT_CHECK: log and bail out of the bring-up task instead of aborting the
// whole box. WiFi runs on the flaky C6; a failure here must cost us WiFi, never
// the USB->caster core (the reason this box exists offline).
#define SOFT_CHECK(expr, what) do {                                            \
        esp_err_t _e = (expr);                                                 \
        if (_e != ESP_OK) {                                                    \
            ESP_LOGE(TAG, "%s failed (%s) — C6 WiFi unavailable, caster core "  \
                     "runs without it", (what), esp_err_to_name(_e));          \
            vTaskDelete(NULL);                                                 \
            return;                                                            \
        }                                                                      \
    } while (0)

static void wifi_task(void *arg)
{
    (void)arg;
    nvs_ready();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    // Brings up the C6 link over SDIO. BLOCKS here (ESP-Hosted transport retry)
    // when the C6 doesn't come up after reset — which is exactly why this runs
    // on its own task and not on app_main.
    SOFT_CHECK(esp_wifi_init(&wcfg), "esp_wifi_init");
    SOFT_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), "wifi event reg");
    SOFT_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL), "ip event reg");

    char ssid[64] = {0}, pass[64] = {0};
    if (!creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no WiFi credentials — set them with: wifiset <ssid> <pass>");
        vTaskDelete(NULL);
        return;  // C6 link is up; idle until provisioned + reboot.
    }

    ESP_LOGI(TAG, "joining '%s'", ssid);
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    SOFT_CHECK(esp_wifi_set_mode(WIFI_MODE_STA), "esp_wifi_set_mode");
    SOFT_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta), "esp_wifi_set_config");
    SOFT_CHECK(esp_wifi_start(), "esp_wifi_start");
    vTaskDelete(NULL);
}

void wifi_sta_start(void)
{
    // Non-blocking: WiFi bring-up runs on its own task. esp_wifi_init() blocks
    // (retrying the ESP-Hosted SDIO transport) whenever the C6 coprocessor
    // doesn't come up after reset; if that ran on app_main it would gate the
    // USB->caster core. Isolating it means a dead C6 costs us WiFi, not the box.
    // 6 KiB: esp_wifi_init + ESP-Hosted transport setup is stack-hungry.
    xTaskCreate(wifi_task, "wifi_start", 6144, NULL, 5, NULL);
}
