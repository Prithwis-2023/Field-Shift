#include "fs_insight.h"
#include "fs_core.h"
#include <stdio.h>
#include <string.h>

/* ---- tunable constants (placeholders: set per crop) ---- */
#define TARGET_PCT        50.0f
#define RAIN_FULL_MM      10.0f
#define RAIN_WEIGHT       0.8f
#define REGION_DRY_PCT    30.0f
#define REGION_DRY_BOOST  1.15f
#define PAIRS_MAX         16
#define MIN_PAIRS         4

static int sg_soil = -2, sg_macro, sg_rain, sg_irr;

static char sg_source[64] = "SIMULATED (mock)";
const char *ins_source(void) { return sg_source; }
void ins_set_source(const char *s) { snprintf(sg_source, sizeof(sg_source), "%s", s); }

static float sg_pl[PAIRS_MAX], sg_pm[PAIRS_MAX];
static int   sg_pn, sg_ph;
static uint32_t sg_last_macro_t;
static float sg_acc;
static int   sg_acc_n;

static void bind(void)
{
    if (sg_soil != -2) return;
    sg_soil  = fs_find("soil");
    sg_macro = fs_find("macro");
    sg_rain  = fs_find("rain");
    sg_irr   = fs_find("irr");
}

static bool get_ok(int ch, float *v, uint32_t *t)
{
    fs_sample_t s;
    if (ch < 0 || !fs_get(ch, &s) || s.quality != FS_Q_OK) return false;
    *v = s.value;
    if (t) *t = s.t_ms;
    return true;
}

static float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

int ins_pair_count(void) { return sg_pn; }

int ins_read_gap(float *raw)
{
    float l, m;
    bind();
    if (!get_ok(sg_soil, &l, NULL) || !get_ok(sg_macro, &m, NULL)) return -1;
    *raw = l - m;
    return 0;
}

int ins_read_corr(float *raw)
{
    float l, m;
    uint32_t tm = 0;
    bind();

    if (get_ok(sg_soil, &l, NULL)) {          /* accumulate local mean between regional updates */
        sg_acc += l;
        sg_acc_n++;
    }
    if (get_ok(sg_macro, &m, &tm) && tm != sg_last_macro_t && sg_acc_n > 0) {
        sg_pl[sg_ph] = sg_acc / (float)sg_acc_n;
        sg_pm[sg_ph] = m;
        sg_ph = (sg_ph + 1) % PAIRS_MAX;
        if (sg_pn < PAIRS_MAX) sg_pn++;
        sg_acc = 0.0f;
        sg_acc_n = 0;
        sg_last_macro_t = tm;
    }

    if (sg_pn < MIN_PAIRS) return -1;

    float mx = 0.0f, my = 0.0f;
    for (int i = 0; i < sg_pn; i++) { mx += sg_pl[i]; my += sg_pm[i]; }
    mx /= (float)sg_pn;
    my /= (float)sg_pn;

    float sxx = 0.0f, syy = 0.0f, sxy = 0.0f;
    for (int i = 0; i < sg_pn; i++) {
        float dx = sg_pl[i] - mx, dy = sg_pm[i] - my;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    if (sxx < 1e-4f || syy < 1e-4f) return -1;   /* no variation: correlation undefined */

    float den = sxx * syy;
    /* sqrt without libm: Newton iterations are plenty for this */
    float r = den;
    for (int k = 0; k < 12; k++) r = 0.5f * (r + den / (r > 1e-9f ? r : 1e-9f));
    *raw = clampf(sxy / (r > 1e-9f ? r : 1e-9f), -1.0f, 1.0f);
    return 0;
}

int ins_read_irr(float *raw)
{
    float l, m = 100.0f, rain = 0.0f;
    bind();
    if (!get_ok(sg_soil, &l, NULL)) return -1;
    (void)get_ok(sg_rain, &rain, NULL);
    bool have_macro = get_ok(sg_macro, &m, NULL);

    float base = clampf((TARGET_PCT - l) / TARGET_PCT, 0.0f, 1.0f);
    float disc = clampf(rain / RAIN_FULL_MM, 0.0f, 1.0f);
    float idx = 100.0f * base * (1.0f - RAIN_WEIGHT * disc);
    if (have_macro && m < REGION_DRY_PCT) idx *= REGION_DRY_BOOST;
    *raw = clampf(idx, 0.0f, 100.0f);
    return 0;
}

const char *ins_level_text(int level)
{
    switch (level) {
    case 1:  return "NO IRRIGATION NEEDED";
    case 2:  return "IRRIGATE SOON";
    case 3:  return "IRRIGATE NOW";
    default: return "WAITING FOR DATA";
    }
}

int ins_verdict(char *reason, int n)
{
    float idx, l = 0.0f, rain = 0.0f;
    bind();
    if (!get_ok(sg_irr, &idx, NULL) || !get_ok(sg_soil, &l, NULL)) {
        snprintf(reason, (size_t)n, "collecting data...");
        return 0;
    }
    (void)get_ok(sg_rain, &rain, NULL);
    snprintf(reason, (size_t)n, "Soil %d%% vs target %d%%, rain %d mm",
             (int)(l + 0.5f), (int)TARGET_PCT, (int)(rain + 0.5f));
    if (idx >= 70.0f) return 3;
    if (idx >= 40.0f) return 2;
    return 1;
}
