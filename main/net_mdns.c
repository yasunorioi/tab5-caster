// net_mdns.c — see net_mdns.h.

#include "net_mdns.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns";

esp_err_t net_mdns_start(const char *hostname, uint16_t caster_port, uint16_t admin_port)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // <hostname>.local + a human-readable instance name.
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) ESP_LOGW(TAG, "hostname_set: %s", esp_err_to_name(err));
    mdns_instance_name_set("tab5-caster");

    // NTRIP caster service. There is no IANA-registered mDNS type for NTRIP;
    // _ntrip._tcp is the de-facto convention. Mountpoints are dynamic, so the
    // TXT just carries a description + base path.
    mdns_txt_item_t ntrip_txt[] = {
        { "desc", "NTRIP caster" },
        { "path", "/" },
    };
    err = mdns_service_add("NTRIP Caster", "_ntrip", "_tcp", caster_port,
                           ntrip_txt, sizeof(ntrip_txt) / sizeof(ntrip_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add _ntrip._tcp:%u failed: %s", (unsigned)caster_port,
                 esp_err_to_name(err));
        return err;
    }

    // Admin / status web UI — best effort (may be disabled on some builds).
    mdns_txt_item_t http_txt[] = { { "path", "/" } };
    err = mdns_service_add("tab5-caster status", "_http", "_tcp", admin_port,
                           http_txt, sizeof(http_txt) / sizeof(http_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add _http._tcp:%u: %s", (unsigned)admin_port,
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "mDNS up: %s.local  _ntrip._tcp:%u  _http._tcp:%u",
             hostname, (unsigned)caster_port, (unsigned)admin_port);
    return ESP_OK;
}
