#include "fs_alarm.h"
#include "fs_core.h"
#include "tal_api.h"
#include <string.h>
#include <stdio.h>

#define FS_MAX_ALARMS 40
#define FS_TICK_MS    100

typedef struct {
    fs_rule_t rule;
    char      text[32];
    bool      active;
    bool      latched;
    bool      pending;
    uint32_t  pend_since;
    uint32_t  raised_ms;
} alarm_t;

static alarm_t      sg_al[FS_MAX_ALARMS];
static int          sg_n;
static MUTEX_HANDLE sg_lock;

static uint32_t now_ms(void) { return (uint32_t)tal_system_get_millisecond(); }

static alarm_t *add_slot(void)
{
    if (sg_n >= FS_MAX_ALARMS) return NULL;
    return &sg_al[sg_n++];
}

/* returns 1 = condition true, 0 = false, -1 = unknown (hold current state) */
static int eval(const alarm_t *a, uint32_t now)
{
    const fs_rule_t *r = &a->rule;
    fs_sample_t smp;
    memset(&smp, 0, sizeof(smp));
    bool got = fs_get(r->channel, &smp);

    switch (r->kind) {
    case FS_AK_RULE:
        if (!got || smp.quality != FS_Q_OK) return -1;
        if (r->cmp == FS_CMP_BELOW) {
            if (a->active) return (smp.value <= r->threshold + r->hysteresis) ? 1 : 0;
            return (smp.value < r->threshold) ? 1 : 0;
        }
        if (a->active) return (smp.value >= r->threshold - r->hysteresis) ? 1 : 0;
        return (smp.value > r->threshold) ? 1 : 0;
    case FS_AK_LOST: {
        uint32_t age = now - (got ? smp.t_ms : 0u);
        return (age > r->hold_ms) ? 1 : 0;
    }
    case FS_AK_FAULT:
        return (got && smp.quality == FS_Q_INVALID) ? 1 : 0;
    default:
        return -1;
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t now = now_ms();
        tal_mutex_lock(sg_lock);
        for (int i = 0; i < sg_n; i++) {
            alarm_t *a = &sg_al[i];
            int c = eval(a, now);
            uint32_t debounce = (a->rule.kind == FS_AK_RULE) ? a->rule.hold_ms : 0u;

            if (c == 1) {
                if (!a->active) {
                    if (!a->pending) { a->pending = true; a->pend_since = now; }
                    if ((now - a->pend_since) >= debounce) {
                        a->active = true;
                        a->pending = false;
                        a->raised_ms = now;
                        if (a->rule.latching) a->latched = true;
                        PR_NOTICE("ALARM %u RAISED: %s", (unsigned)a->rule.code, a->rule.text);
                    }
                }
            } else if (c == 0) {
                a->pending = false;
                if (a->active) {
                    a->active = false;
                    PR_NOTICE("ALARM %u CLEARED: %s", (unsigned)a->rule.code, a->rule.text);
                }
            } else {
                a->pending = false;
            }
        }
        tal_mutex_unlock(sg_lock);
        tal_system_sleep(FS_TICK_MS);
    }
}

void fs_alarm_start(void)
{
    memset(sg_al, 0, sizeof(sg_al));
    sg_n = 0;

    int cnt = fs_count();
    for (int ch = 0; ch < cnt; ch++) {
        const fs_sensor_t *d = fs_def(ch);

        for (int k = 0; k < d->n_rules; k++) {
            alarm_t *a = add_slot();
            if (!a) break;
            const fs_rule_def_t *rd = &d->rules[k];
            a->rule.code       = rd->code;
            a->rule.text       = rd->text;
            a->rule.channel    = ch;
            a->rule.kind       = FS_AK_RULE;
            a->rule.cmp        = rd->cmp;
            a->rule.threshold  = rd->threshold;
            a->rule.hysteresis = rd->hysteresis;
            a->rule.hold_ms    = rd->hold_ms;
            a->rule.sev        = rd->sev;
            a->rule.latching   = rd->latching;
        }

        /* every sensor gets health alarms automatically */
        alarm_t *a = add_slot();
        if (a) {
            snprintf(a->text, sizeof(a->text), "%s LOST", d->label);
            a->rule.code     = (uint16_t)(2100 + ch);
            a->rule.text     = a->text;
            a->rule.channel  = ch;
            a->rule.kind     = FS_AK_LOST;
            a->rule.hold_ms  = d->lost_ms ? d->lost_ms : FS_DEFAULT_LOST_MS;
            a->rule.sev      = FS_SEV_CAUTION;
            a->rule.latching = false;
        }
        a = add_slot();
        if (a) {
            snprintf(a->text, sizeof(a->text), "%s FAULT", d->label);
            a->rule.code     = (uint16_t)(2200 + ch);
            a->rule.text     = a->text;
            a->rule.channel  = ch;
            a->rule.kind     = FS_AK_FAULT;
            a->rule.sev      = FS_SEV_WARNING;
            a->rule.latching = true;
        }
    }

    if (tal_mutex_create_init(&sg_lock) != OPRT_OK) {
        PR_ERR("alarm mutex failed");
        sg_n = 0;
        return;
    }
    PR_NOTICE("alarm table: %d alarms for %d sensors", sg_n, cnt);

    THREAD_HANDLE h = NULL;
    THREAD_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = 1024 * 4;
    cfg.priority   = THREAD_PRIO_2;
    cfg.thrdname   = "fs_alarm";
    tal_thread_create_and_start(&h, NULL, NULL, alarm_task, NULL, &cfg);
}

int fs_alarm_count(void) { return sg_n; }

bool fs_alarm_get(int i, fs_alarm_info_t *out)
{
    if (i < 0 || i >= sg_n || !out) return false;
    tal_mutex_lock(sg_lock);
    out->rule      = &sg_al[i].rule;
    out->active    = sg_al[i].active;
    out->latched   = sg_al[i].latched;
    out->raised_ms = sg_al[i].raised_ms;
    tal_mutex_unlock(sg_lock);
    return true;
}

int fs_alarm_shown_count(void)
{
    int n = 0;
    if (sg_n <= 0) return 0;
    tal_mutex_lock(sg_lock);
    for (int i = 0; i < sg_n; i++) {
        if (sg_al[i].active || sg_al[i].latched) n++;
    }
    tal_mutex_unlock(sg_lock);
    return n;
}

int fs_alarm_top(void)
{
    int best = -1;
    if (sg_n <= 0) return -1;
    tal_mutex_lock(sg_lock);
    for (int i = 0; i < sg_n; i++) {
        if (!(sg_al[i].active || sg_al[i].latched)) continue;
        if (best < 0 ||
            sg_al[i].rule.sev > sg_al[best].rule.sev ||
            (sg_al[i].rule.sev == sg_al[best].rule.sev &&
             sg_al[i].raised_ms > sg_al[best].raised_ms)) {
            best = i;
        }
    }
    tal_mutex_unlock(sg_lock);
    return best;
}

int fs_alarm_channel_sev(int ch)
{
    int sev = -1;
    if (sg_n <= 0) return -1;
    tal_mutex_lock(sg_lock);
    for (int i = 0; i < sg_n; i++) {
        if (sg_al[i].rule.channel != ch) continue;
        if (!(sg_al[i].active || sg_al[i].latched)) continue;
        if ((int)sg_al[i].rule.sev > sev) sev = (int)sg_al[i].rule.sev;
    }
    tal_mutex_unlock(sg_lock);
    return sev;
}

void fs_alarm_ack(void)
{
    if (sg_n <= 0) return;
    tal_mutex_lock(sg_lock);
    for (int i = 0; i < sg_n; i++) sg_al[i].latched = false;
    tal_mutex_unlock(sg_lock);
}

int fs_alarm_set_threshold(uint16_t code, float thr)
{
    int rc = -1;
    if (sg_n <= 0) return -1;
    tal_mutex_lock(sg_lock);
    for (int i = 0; i < sg_n; i++) {
        if (sg_al[i].rule.code == code && sg_al[i].rule.kind == FS_AK_RULE) {
            sg_al[i].rule.threshold = thr;
            rc = 0;
            break;
        }
    }
    tal_mutex_unlock(sg_lock);
    return rc;
}
