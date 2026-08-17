// web_server.c — see web_server.h. Read-only HTTP status server on esp_http_server.

#include "web_server.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "wifi_sta.h"
#include "usb_cdc_source.h"
#include "upstream.h"
#include "rtcm_monitor.h"
#include "caster.h"

static const char *TAG = "web";

static httpd_handle_t s_httpd;   // NULL until started; guards idempotency

// Cap the RTCM type array we emit so a full 48-type monitor can't blow the JSON
// past a sane size. The obs+eph set the caster needs is ~16 types.
#define JSON_MAX_TYPES 24

// ── helpers ──────────────────────────────────────────────────────────────────

// Escape `in` for embedding in a JSON string literal, into `out` (always
// NUL-terminated, truncated to fit). Control chars are dropped; SSIDs and the
// upstream last_msg can carry arbitrary bytes, so this must not be skipped.
static void json_str(char *out, size_t out_max, const char *in)
{
    size_t o = 0;
    if (out_max == 0) return;
    for (size_t i = 0; in && in[i] && o + 2 < out_max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
        // else: other control chars dropped
    }
    out[o] = '\0';
}

// ── GET /api/status ──────────────────────────────────────────────────────────
// Built by chunked sends so no single large buffer is needed. Stateless: raw
// counters only; the page computes rates from successive polls.
static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t         w;  wifi_sta_status(&w);
    usb_cdc_status_t      u;  usb_cdc_source_status(&u);
    upstream_status_t     up; upstream_status(&up);
    rtcm_mon_stats_t      m;  rtcm_monitor_get(&m);
    caster_source_stats_t cs; caster_source_stats(&cs);

    const int64_t now = esp_timer_get_time();
    const long long ms_since = m.last_frame_us ? (now - m.last_frame_us) / 1000 : -1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char line[320];
    char esc[128];

    httpd_resp_sendstr_chunk(req, "{");

    // wifi
    json_str(esc, sizeof esc, w.ssid);
    snprintf(line, sizeof line,
             "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},",
             w.connected ? "true" : "false", esc, w.ip);
    httpd_resp_sendstr_chunk(req, line);

    // usb
    json_str(esc, sizeof esc, u.topo);
    snprintf(line, sizeof line,
             "\"usb\":{\"host\":%s,\"attached\":%s,\"open\":%s,"
             "\"vid\":%u,\"pid\":%u,\"num_if\":%u,\"cur_itf\":%d,"
             "\"stream_itf\":%d,\"topo\":\"%s\"},",
             u.host_installed ? "true" : "false",
             u.device_attached ? "true" : "false",
             u.cdc_open ? "true" : "false",
             u.vid, u.pid, u.num_interfaces,
             (int)(int8_t)u.cur_itf, (int)(int8_t)u.stream_itf, esc);
    httpd_resp_sendstr_chunk(req, line);

    // upstream — split across two chunks so neither snprintf can overrun line[]:
    // host(64)+mount(32)+escaped last_msg(128) together exceed one buffer.
    // host/mount are box-set (no untrusted bytes); last_msg is escaped.
    snprintf(line, sizeof line,
             "\"upstream\":{\"provisioned\":%s,\"connected\":%s,\"host\":\"%s\","
             "\"port\":%u,\"mount\":\"%s\",",
             up.provisioned ? "true" : "false",
             up.connected ? "true" : "false",
             up.host, up.port, up.mount);
    httpd_resp_sendstr_chunk(req, line);
    json_str(esc, sizeof esc, up.last_msg);
    snprintf(line, sizeof line,
             "\"bytes_sent\":%llu,\"reconnects\":%lu,\"last_msg\":\"%s\"},",
             (unsigned long long)up.bytes_sent,
             (unsigned long)up.reconnects, esc);
    httpd_resp_sendstr_chunk(req, line);

    // caster
    snprintf(line, sizeof line,
             "\"caster\":{\"running\":%s,\"source_present\":%s,\"rtcm_detected\":%s,"
             "\"bytes_in\":%llu,\"clients\":%u},",
             cs.running ? "true" : "false",
             cs.source_present ? "true" : "false",
             cs.rtcm_detected ? "true" : "false",
             (unsigned long long)cs.bytes_in, cs.client_count);
    httpd_resp_sendstr_chunk(req, line);

    // rtcm (counters + type histogram)
    snprintf(line, sizeof line,
             "\"rtcm\":{\"bytes\":%llu,\"valid\":%llu,\"crc_fails\":%llu,"
             "\"ms_since_last\":%lld,\"types\":[",
             (unsigned long long)m.total_bytes,
             (unsigned long long)m.valid_frames,
             (unsigned long long)m.crc_fails, ms_since);
    httpd_resp_sendstr_chunk(req, line);

    uint32_t n = m.type_count < JSON_MAX_TYPES ? m.type_count : JSON_MAX_TYPES;
    for (uint32_t i = 0; i < n; i++) {
        snprintf(line, sizeof line, "%s{\"t\":%u,\"n\":%lu}",
                 i ? "," : "", m.types[i].type, (unsigned long)m.types[i].count);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]}}");

    httpd_resp_sendstr_chunk(req, NULL);   // end of response
    return ESP_OK;
}

// ── GET / ────────────────────────────────────────────────────────────────────
// Self-contained dashboard: polls /api/status once a second and renders it.
// No external assets (offline box), matches the panel's dark background.
static const char INDEX_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>tab5-caster</title><style>"
"body{margin:0;font:14px/1.5 system-ui,sans-serif;background:#081420;color:#cfe3f2}"
"header{padding:14px 18px;background:#0d1f30;border-bottom:1px solid #1b3550}"
"header b{font-size:17px}header span{color:#6b8bab;margin-left:8px}"
".wrap{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));padding:16px}"
".card{background:#0d1f30;border:1px solid #1b3550;border-radius:10px;padding:14px 16px}"
".card h2{margin:0 0 10px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#6b8bab}"
".row{display:flex;justify-content:space-between;gap:12px;padding:3px 0}"
".row .k{color:#8fb0cd}.row .v{font-variant-numeric:tabular-nums;text-align:right}"
".ok{color:#5fe08a}.bad{color:#ff6b6b}.dim{color:#5a7794}"
".pill{display:inline-block;padding:1px 8px;border-radius:20px;font-size:12px}"
".pill.ok{background:#123d24}.pill.bad{background:#3d1414}"
".types{display:flex;flex-wrap:wrap;gap:6px;margin-top:4px}"
".types span{background:#12293e;border-radius:6px;padding:2px 7px;font-size:12px;font-variant-numeric:tabular-nums}"
"footer{padding:10px 18px;color:#4a688a;font-size:12px}"
"</style></head><body>"
"<header><b>tab5-caster</b><span id=host></span></header>"
"<div class=wrap>"
"<div class=card><h2>Caster</h2><div id=caster></div></div>"
"<div class=card><h2>RTCM3 in</h2><div id=rtcm></div></div>"
"<div class=card><h2>Cloud upstream</h2><div id=up></div></div>"
"<div class=card><h2>Mosaic / USB</h2><div id=usb></div></div>"
"<div class=card><h2>WiFi</h2><div id=wifi></div></div>"
"</div>"
"<footer id=foot>connecting…</footer>"
"<script>"
"var pv=null,pt=0;"
"function b(x){return x?'<span class=\"pill ok\">yes</span>':'<span class=\"pill bad\">no</span>'}"
"function r(k,v){return '<div class=row><span class=k>'+k+'</span><span class=v>'+v+'</span></div>'}"
"function num(n){return (n||0).toLocaleString()}"
"function tick(){fetch('/api/status').then(function(x){return x.json()}).then(function(d){"
"var now=Date.now();"
"var rate='';"
"if(pv){var dt=(now-pt)/1000;var db=d.rtcm.bytes-pv.rtcm.bytes;if(dt>0)rate=Math.max(0,Math.round(db/dt))+' B/s'}"
"pv=d;pt=now;"
"document.getElementById('host').textContent=d.wifi.ip?('@ '+d.wifi.ip):'';"
"var age=d.rtcm.ms_since_last;var fresh=age>=0&&age<5000;"
"document.getElementById('caster').innerHTML="
"r('listener',b(d.caster.running))+r('source /MOSAIC',b(d.caster.source_present))+"
"r('rovers',num(d.caster.clients))+r('fed to caster',num(d.caster.bytes_in)+' B');"
"document.getElementById('rtcm').innerHTML="
"r('stream',b(fresh))+r('rate',rate||'<span class=dim>—</span>')+"
"r('valid frames',num(d.rtcm.valid))+r('CRC fails',(d.rtcm.crc_fails? '<span class=bad>':'<span class=ok>')+num(d.rtcm.crc_fails)+'</span>')+"
"r('last frame',age<0?'<span class=dim>never</span>':(age<5000?'<span class=ok>'+age+' ms</span>':'<span class=bad>'+num(age)+' ms</span>'))+"
"'<div class=types>'+(d.rtcm.types||[]).map(function(t){return '<span>'+t.t+'·'+num(t.n)+'</span>'}).join('')+'</div>';"
"document.getElementById('up').innerHTML="
"r('provisioned',b(d.upstream.provisioned))+r('connected',b(d.upstream.connected))+"
"(d.upstream.provisioned?r('target',(d.upstream.host||'?')+':'+d.upstream.port+'/'+d.upstream.mount):'')+"
"r('sent',num(d.upstream.bytes_sent)+' B')+r('reconnects',num(d.upstream.reconnects))+"
"r('state','<span class=dim>'+(d.upstream.last_msg||'')+'</span>');"
"document.getElementById('usb').innerHTML="
"r('host',b(d.usb.host))+r('attached',b(d.usb.attached))+r('cdc open',b(d.usb.open))+"
"r('device',d.usb.vid?(hex(d.usb.vid)+':'+hex(d.usb.pid)):'<span class=dim>—</span>')+"
"r('stream itf',d.usb.stream_itf<0?'<span class=dim>—</span>':d.usb.stream_itf)+"
"r('topology','<span class=dim>'+(d.usb.topo||'')+'</span>');"
"document.getElementById('wifi').innerHTML="
"r('connected',b(d.wifi.connected))+r('SSID',d.wifi.ssid||'<span class=dim>—</span>')+"
"r('IP',d.wifi.ip||'<span class=dim>—</span>');"
"document.getElementById('foot').textContent='updated '+new Date().toLocaleTimeString();"
"}).catch(function(e){document.getElementById('foot').textContent='unreachable — retrying…'})}"
"function hex(n){return ('000'+n.toString(16).toUpperCase()).slice(-4)}"
"tick();setInterval(tick,1000);"
"</script></body></html>";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ── start ────────────────────────────────────────────────────────────────────
esp_err_t web_server_start(void)
{
    if (s_httpd) return ESP_OK;   // idempotent

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_ADMIN_PORT;
    cfg.lru_purge_enable = true;   // reclaim the oldest idle socket under pressure
    cfg.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    const httpd_uri_t index_uri  = { .uri = "/",           .method = HTTP_GET, .handler = index_get };
    const httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get };
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &index_uri);

    ESP_LOGI(TAG, "status web UI on :%u  (http://rtk.local:%u/)",
             WEB_ADMIN_PORT, WEB_ADMIN_PORT);
    return ESP_OK;
}
