// backlight.c — see backlight.h.

#include "backlight.h"
#include "display.h"

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "rtcm_monitor.h"

static const char *TAG = "backlight";

// Tuning. Percentages are LEDC duty (display_backlight units).
#define BL_FLOOR        20   // always-safe level; the receiver comes up here
#define BL_TARGET       80   // desired brightness when the supply allows it
#define BL_STEP         10   // climb increment per step
#define BL_WARMUP_S     5    // healthy streaming seconds before the first climb
#define BL_CLIMB_EVERY  3    // raise one step every N healthy seconds
#define BL_FRESH_US     3000000  // RTCM3 "fresh" if a valid frame within 3 s
#define BL_TICK_US      1000000  // 1 Hz

static int      s_cur;                 // current applied brightness
static int      s_ceiling = 100;       // max % believed safe this session
static int      s_healthy;             // consecutive healthy-stream ticks
static uint64_t s_prev_frames;         // to detect valid-frame advance
static bool     s_was_streaming;       // stream state last tick
static bool     s_manual;              // console override active
static esp_timer_handle_t s_timer;

static void apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == s_cur) return;
    s_cur = pct;
    display_backlight(pct);
}

static void tick(void *arg)
{
    (void)arg;
    if (s_manual) return;

    rtcm_mon_stats_t ms;
    rtcm_monitor_get(&ms);
    int64_t now = esp_timer_get_time();

    bool advancing = ms.valid_frames > s_prev_frames;
    bool fresh = ms.last_frame_us && (now - ms.last_frame_us) < BL_FRESH_US;
    bool streaming = fresh && advancing;   // RTCM3 actively arriving
    s_prev_frames = ms.valid_frames;

    // Brownout: we were streaming, now the stream is gone. The current
    // brightness couldn't be sustained on this supply — cap the ceiling below
    // it and fall to the floor so the receiver has headroom to re-latch.
    if (s_was_streaming && !streaming) {
        // Back off well below where it failed: the brownout edge is noisy (a
        // level can hold for seconds before dropping), so a wide margin makes it
        // converge to a stable brightness in one or two cycles, not a slow walk.
        int newceil = s_cur - 3 * BL_STEP;
        if (newceil < BL_FLOOR) newceil = BL_FLOOR;
        if (newceil < s_ceiling) s_ceiling = newceil;
        ESP_LOGW(TAG, "stream dropped at %d%% — floor %d%%, ceiling now %d%%",
                 s_cur, BL_FLOOR, s_ceiling);
        apply(BL_FLOOR);
        s_healthy = 0;
        s_was_streaming = false;
        return;
    }

    if (!streaming) {
        // Not latched yet (boot / between sweeps) — hold the floor so the
        // receiver can always enumerate and start output.
        apply(BL_FLOOR);
        s_healthy = 0;
        return;
    }

    // Streaming healthy. Climb slowly toward the target, bounded by the learned
    // ceiling, after a warmup so a brief first latch doesn't provoke a climb.
    s_was_streaming = true;
    if (s_healthy < 100000) s_healthy++;

    int goal = BL_TARGET < s_ceiling ? BL_TARGET : s_ceiling;
    if (s_healthy >= BL_WARMUP_S && (s_healthy % BL_CLIMB_EVERY) == 0
        && s_cur < goal) {
        int next = s_cur + BL_STEP;
        if (next > goal) next = goal;
        apply(next);
        ESP_LOGI(TAG, "backlight -> %d%% (goal %d%%)", next, goal);
    }
}

void backlight_auto_start(void)
{
    s_cur = -1;              // force the first apply() to write
    apply(BL_FLOOR);        // safe from t0, before the first tick

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "backlight",
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, BL_TICK_US) != ESP_OK) {
        ESP_LOGE(TAG, "timer start failed — backlight fixed at %d%%", BL_FLOOR);
        return;
    }
    ESP_LOGI(TAG, "adaptive backlight up (floor %d%%, target %d%%)",
             BL_FLOOR, BL_TARGET);
}

void backlight_manual(int percent)
{
    s_manual = true;
    apply(percent);
    ESP_LOGI(TAG, "manual backlight %d%% (auto suspended)", percent);
}
