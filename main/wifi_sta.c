// wifi_sta.c — see wifi_sta.h. WiFi STA over the C6 (ESP-Hosted) with a SoftAP
// browser setup portal for first-time (or re-)provisioning. No phone app, no
// baked-in credentials.

#include "wifi_sta.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"        // provided by esp_wifi_remote (radio runs on the C6)
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "caster.h"

static const char *TAG = "wifi";

#define NVS_NS       "wifi"     // namespace for stored credentials
#define AP_SSID      "rtk-setup"
#define AP_CHANNEL   1
#define AP_MAX_CONN  2

static bool s_caster_started;
static httpd_handle_t s_httpd;

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

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "credentials erased — rebooting into setup portal");
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
        if (!s_caster_started) { s_caster_started = true; caster_start(); }
    }
}

// ── setup portal (SoftAP HTTP) ───────────────────────────────────────────────

// Minimal url-decode in place (handles %XX and '+'). Returns dst.
static char *url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return s;
}

static esp_err_t root_get(httpd_req_t *req)
{
    // Scan visible APs (STA iface is up in APSTA mode).
    uint16_t n = 0;
    wifi_ap_record_t aps[20];
    uint16_t cap = sizeof(aps) / sizeof(aps[0]);
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_records(&cap, aps);
    esp_wifi_scan_get_ap_num(&n);
    if (n > cap) n = cap;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>RTK caster setup</title>"
        "<h2>RTK caster WiFi setup</h2>"
        "<form method=POST action=/save>"
        "<p>Network:<br><select name=ssid style='width:100%;padding:6px'>");
    for (uint16_t i = 0; i < n; i++) {
        // De-dup consecutive same SSIDs cheaply (scan is roughly sorted by RSSI).
        if (i && strcmp((char *)aps[i].ssid, (char *)aps[i - 1].ssid) == 0) continue;
        if (aps[i].ssid[0] == '\0') continue;
        httpd_resp_sendstr_chunk(req, "<option>");
        httpd_resp_sendstr_chunk(req, (char *)aps[i].ssid);
        httpd_resp_sendstr_chunk(req, "</option>");
    }
    httpd_resp_sendstr_chunk(req,
        "</select></p>"
        "<p>Password:<br><input name=pass type=password style='width:100%;padding:6px'></p>"
        "<p><button style='padding:8px 16px'>Save &amp; connect</button></p>"
        "</form>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[got] = '\0';

    // Parse ssid=...&pass=... (order-independent).
    char ssid[64] = {0}, pass[64] = {0};
    char *tok = strtok(body, "&");
    while (tok) {
        if (strncmp(tok, "ssid=", 5) == 0) strlcpy(ssid, url_decode(tok + 5), sizeof(ssid));
        else if (strncmp(tok, "pass=", 5) == 0) strlcpy(pass, url_decode(tok + 5), sizeof(pass));
        tok = strtok(NULL, "&");
    }
    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<p>SSID required. <a href=/>back</a>");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "saving credentials for SSID '%s' — rebooting to connect", ssid);
    creds_save(ssid, pass);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<h2>Saved.</h2><p>Rebooting to join the network. This AP will disappear.</p>");
    // Let the response flush before restarting.
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    ESP_LOGI(TAG, "setup portal up: join WiFi '%s', open http://192.168.4.1/", AP_SSID);
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

void wifi_sta_start(void)
{
    nvs_ready();

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));   // brings up the C6 link over SDIO
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    char ssid[64] = {0}, pass[64] = {0};
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "stored credentials found — joining '%s'", ssid);
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGW(TAG, "no stored credentials — starting setup portal");
        wifi_config_t ap = {0};
        strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(AP_SSID);
        ap.ap.channel = AP_CHANNEL;
        ap.ap.max_connection = AP_MAX_CONN;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        // APSTA: SoftAP serves the portal; STA iface lets us scan for networks.
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        portal_start();
    }
}
