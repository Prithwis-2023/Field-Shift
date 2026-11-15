#include "fs_core.h"
#include "tal_api.h"
#include <string.h>

typedef struct {
    fs_sample_t last;
    uint32_t    last_poll_ms;
    uint32_t    t_hs;
    uint32_t    t_hl;
    uint16_t    hs_head, hs_cnt;
    uint16_t    hl_head, hl_cnt;
    uint16_t    hs[FS_H_LEN];
    uint16_t    hl[FS_H_LEN];
    bool        polled;
} fs_slot_t;

static const fs_sensor_t *sg_s;
static int                sg_n;
static fs_slot_t          sg_slot[FS_MAX_CHANNELS];
static MUTEX_HANDLE       sg_lock;

static uint32_t now_ms(void) { return (uint32_t)tal_system_get_millisecond(); }

/* history is stored as 16-bit values scaled to the channel's valid range */
static uint16_t q16(const fs_sensor_t *d, float v)
{
    float f = (v - d->min_valid) / (d->max_valid - d->min_valid);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return (uint16_t)(f * 65000.0f);
}

static float dq16(const fs_sensor_t *d, uint16_t q)
{
    return d->min_valid + ((float)q / 65000.0f) * (d->max_valid - d->min_valid);
}

static void hist_push(uint16_t *buf, uint16_t *head, uint16_t *cnt, uint16_t q)
{
    buf[*head] = q;
    *head = (uint16_t)((*head + 1) % FS_H_LEN);
    if (*cnt < FS_H_LEN) (*cnt)++;
}

int fs_init(const fs_sensor_t *sensors, int count)
{
    if (!sensors || count <= 0 || count > FS_MAX_CHANNELS) return -1;
    for (int i = 0; i < count; i++) {
        if (!(sensors[i].max_valid > sensors[i].min_valid)) return -1;
    }
    memset(sg_slot, 0, sizeof(sg_slot));
    sg_s = sensors;
    sg_n = count;
    return (tal_mutex_create_init(&sg_lock) == OPRT_OK) ? 0 : -1;
}

int fs_count(void) { return sg_n; }

const fs_sensor_t *fs_def(int ch)
{
    return (ch >= 0 && ch < sg_n) ? &sg_s[ch] : NULL;
}

int fs_find(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < sg_n; i++) {
        if (strcmp(sg_s[i].key, key) == 0) return i;
    }
    return -1;
}

uint32_t fs_hist_period_ms(fs_hist_t which)
{
    return (which == FS_HIST_SHORT) ? FS_HS_PERIOD_MS : FS_HL_PERIOD_MS;
}

int fs_publish(int ch, float raw)
{
    if (ch < 0 || ch >= sg_n) return -1;
    const fs_sensor_t *d = &sg_s[ch];
    float sc = (d->cal_scale > 1e-12f || d->cal_scale < -1e-12f) ? d->cal_scale : 1.0f;
    float v = raw * sc + d->cal_offset;
    uint32_t t = now_ms();
    int rc = 0;

    tal_mutex_lock(sg_lock);
    fs_slot_t *s = &sg_slot[ch];
    if (v != v || v < d->min_valid || v > d->max_valid) {
        s->last.quality = FS_Q_INVALID;
        s->last.t_ms = t;
        rc = -1;
    } else {
        uint16_t q = q16(d, v);
        s->last.value = v;
        s->last.t_ms = t;
        s->last.quality = FS_Q_OK;
        if (s->hs_cnt == 0 || (t - s->t_hs) >= FS_HS_PERIOD_MS) {
            hist_push(s->hs, &s->hs_head, &s->hs_cnt, q);
            s->t_hs = t;
        }
        if (s->hl_cnt == 0 || (t - s->t_hl) >= FS_HL_PERIOD_MS) {
            hist_push(s->hl, &s->hl_head, &s->hl_cnt, q);
            s->t_hl = t;
        }
    }
    tal_mutex_unlock(sg_lock);
    return rc;
}

bool fs_get(int ch, fs_sample_t *out)
{
    if (ch < 0 || ch >= sg_n || !out) return false;
    const fs_sensor_t *d = &sg_s[ch];

    tal_mutex_lock(sg_lock);
    *out = sg_slot[ch].last;
    tal_mutex_unlock(sg_lock);

    if (out->quality == FS_Q_NO_DATA) return false;
    if (out->quality == FS_Q_OK && d->max_age_ms &&
        (now_ms() - out->t_ms) > d->max_age_ms) {
        out->quality = FS_Q_STALE;
    }
    return true;
}

int fs_hist_get(int ch, fs_hist_t which, float *out, int max)
{
    if (ch < 0 || ch >= sg_n || !out || max <= 0) return 0;
    const fs_sensor_t *d = &sg_s[ch];

    tal_mutex_lock(sg_lock);
    fs_slot_t *s = &sg_slot[ch];
    const uint16_t *buf = (which == FS_HIST_SHORT) ? s->hs : s->hl;
    int head = (which == FS_HIST_SHORT) ? s->hs_head : s->hl_head;
    int cnt  = (which == FS_HIST_SHORT) ? s->hs_cnt  : s->hl_cnt;
    int n = (cnt < max) ? cnt : max;
    int start = (head + FS_H_LEN - n) % FS_H_LEN;
    for (int i = 0; i < n; i++) out[i] = dq16(d, buf[(start + i) % FS_H_LEN]);
    tal_mutex_unlock(sg_lock);
    return n;
}

/* the generic poller: runs every sensor row that has a read() callback */
static void poll_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t t = now_ms();
        for (int i = 0; i < sg_n; i++) {
            const fs_sensor_t *d = &sg_s[i];
            fs_slot_t *s = &sg_slot[i];
            if (!d->read || d->poll_ms == 0) continue;
            if (s->polled && (t - s->last_poll_ms) < d->poll_ms) continue;
            s->polled = true;
            s->last_poll_ms = t;
            float raw = 0.0f;
            if (d->read(&raw) == 0) fs_publish(i, raw);
        }
        tal_system_sleep(50);
    }
}

int fs_start(void)
{
    THREAD_HANDLE h = NULL;
    THREAD_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = 1024 * 4;
    cfg.priority   = THREAD_PRIO_2;
    cfg.thrdname   = "fs_poll";
    return (tal_thread_create_and_start(&h, NULL, NULL, poll_task, NULL, &cfg) == OPRT_OK) ? 0 : -1;
}
