// upstream.c — see upstream.h. NTRIP v1 SOURCE client to the cloud caster.

#include "upstream.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "rtcm_sink.h"

static const char *TAG = "upstream";

#define NVS_NS            "upstream"
#define SOURCE_AGENT      "NTRIP tab5-caster/1.0"
#define CONNECT_TIMEOUT_S 10
#define SEND_TIMEOUT_S    10
#define BACKOFF_MAX_S     30
#define IDLE_POLL_S       5      // re-check NVS this often while unprovisioned

static SemaphoreHandle_t s_lock;
static upstream_status_t s_st;   // guarded by s_lock
static bool s_started;

// ── status helpers ───────────────────────────────────────────────────────────
static void st_set_msg(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_msg, msg, sizeof(s_st.last_msg));
    xSemaphoreGive(s_lock);
}

void upstream_status(upstream_status_t *out)
{
    if (!out) return;
    // The status UI may poll before upstream_start() (GOT_IP) has created the
    // lock — report an empty (unprovisioned) status rather than taking a NULL sem.
    if (!s_lock) { *out = (upstream_status_t){0}; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

// ── NVS credentials ──────────────────────────────────────────────────────────
static const char *skip_slash(const char *m) { return (m && m[0] == '/') ? m + 1 : m; }

void upstream_set_creds(const char *host, uint16_t port, const char *mount, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "host", host ? host : ""));
    ESP_ERROR_CHECK(nvs_set_u16(h, "port", port));
    ESP_ERROR_CHECK(nvs_set_str(h, "mount", skip_slash(mount ? mount : "")));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass ? pass : ""));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "creds saved: %s:%u /%s — will connect on next cycle",
             host ? host : "", port, skip_slash(mount ? mount : ""));
}

void upstream_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "upstream credentials erased");
}

// Load creds into locals + publish host/port/mount into status. Returns false if
// unprovisioned (no host).
static bool creds_load(char *host, size_t hl, uint16_t *port, char *mount, size_t ml,
                       char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n;
    n = hl; bool ok = nvs_get_str(h, "host", host, &n) == ESP_OK;
    ok = ok && nvs_get_u16(h, "port", port) == ESP_OK;
    n = ml; ok = ok && nvs_get_str(h, "mount", mount, &n) == ESP_OK;
    n = pl; if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    if (!ok || host[0] == '\0') return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.provisioned = true;
    strlcpy(s_st.host, host, sizeof(s_st.host));
    s_st.port = *port;
    strlcpy(s_st.mount, mount, sizeof(s_st.mount));
    xSemaphoreGive(s_lock);
    return true;
}

// ── socket helpers ───────────────────────────────────────────────────────────
static int connect_caster(const char *host, uint16_t port)
{
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%u", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || res == NULL) {
        st_set_msg("DNS resolve failed");
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) { st_set_msg("connect failed"); close(sock); return -1; }

    // After connect, tighten the send timeout so a stalled cloud can't wedge us.
    tv.tv_sec = SEND_TIMEOUT_S;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return sock;
}

static bool send_all(int sock, const uint8_t *p, size_t n)
{
    while (n) {
        int s = send(sock, p, n, 0);
        if (s <= 0) return false;
        p += s; n -= (size_t)s;
    }
    return true;
}

// NTRIP v1 SOURCE login; returns true if the caster accepted us ("OK"/200).
static bool source_handshake(int sock, const char *mount, const char *pass)
{
    char req[256];
    int len = snprintf(req, sizeof(req),
                       "SOURCE %s /%s\r\nSource-Agent: %s\r\n\r\n",
                       pass, mount, SOURCE_AGENT);
    if (len <= 0 || !send_all(sock, (const uint8_t *)req, (size_t)len)) {
        st_set_msg("handshake send failed");
        return false;
    }
    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    if (r <= 0) { st_set_msg("no handshake response"); return false; }
    resp[r] = '\0';
    // v1 caster replies "OK\r\n"; v2 would be "HTTP/1.1 200 OK".
    if (strstr(resp, "OK") != NULL && strstr(resp, "ERROR") == NULL) {
        return true;
    }
    // Trim to first line for the status message.
    char *nl = strpbrk(resp, "\r\n");
    if (nl) *nl = '\0';
    char msg[80];
    snprintf(msg, sizeof(msg), "rejected: %.60s", resp);
    st_set_msg(msg);
    ESP_LOGW(TAG, "SOURCE rejected: %s", resp);
    return false;
}

// ── task ─────────────────────────────────────────────────────────────────────
static void stream_loop(int sock)
{
    // Fresh connection: drop any backlog queued while we were down.
    rtcm_upstream_reset();
    uint8_t buf[1024];
    for (;;) {
        size_t n = rtcm_upstream_read(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (n == 0) continue;   // no RTCM this second; keep the link open
        if (!send_all(sock, buf, n)) {
            st_set_msg("send failed — reconnecting");
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.bytes_sent += n;
        xSemaphoreGive(s_lock);
    }
}

static void upstream_task(void *arg)
{
    (void)arg;
    int backoff = 1;
    for (;;) {
        char host[64], mount[32], pass[64];
        uint16_t port = 0;
        if (!creds_load(host, sizeof(host), &port, mount, sizeof(mount), pass, sizeof(pass))) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.provisioned = false; s_st.connected = false;
            xSemaphoreGive(s_lock);
            st_set_msg("idle — set creds with 'upstreamset'");
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_S * 1000));
            continue;
        }

        st_set_msg("connecting");
        int sock = connect_caster(host, port);
        if (sock < 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.connected = false; s_st.reconnects++;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
            backoff = (backoff * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff * 2;
            continue;
        }

        if (!source_handshake(sock, mount, pass)) {
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
            backoff = (backoff * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff * 2;
            continue;
        }

        ESP_LOGI(TAG, "connected to %s:%u /%s — streaming base RTCM3", host, port, mount);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.connected = true;
        xSemaphoreGive(s_lock);
        st_set_msg("streaming");
        backoff = 1;                 // reset backoff on a good connection

        stream_loop(sock);           // returns on disconnect

        close(sock);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.connected = false; s_st.reconnects++;
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
        backoff = (backoff * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff * 2;
    }
}

void upstream_start(void)
{
    if (s_started) return;
    s_started = true;
    s_lock = xSemaphoreCreateMutex();
    strlcpy(s_st.last_msg, "starting", sizeof(s_st.last_msg));
    // 6 KiB: getaddrinfo + TLS-free socket path; comfortable headroom.
    xTaskCreate(upstream_task, "upstream", 6144, NULL, 4, NULL);
}
