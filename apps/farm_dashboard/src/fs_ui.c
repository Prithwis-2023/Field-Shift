#include "fs_ui.h"
#include "fs_core.h"
#include "fs_alarm.h"
#include "fs_forecast.h"
#include "fs_lvcompat.h"
#include "fs_ui_insight.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdint.h>

LV_IMAGE_DECLARE(ghibli_bg);
LV_IMAGE_DECLARE(leaf_sprite);

#define UI_MAX      FS_MAX_CHANNELS
#define BANNER_H    32
#define PROJ_STEPS  24
#define CH_PTS      (FS_H_LEN + PROJ_STEPS)

static lv_obj_t *sg_tv, *sg_tile_graph, *sg_tile_dash, *sg_tile_insight;
static lv_obj_t *sg_panel[UI_MAX], *sg_val[UI_MAX];
static lv_obj_t *sg_banner, *sg_banner_lbl;
static int sg_last_top = -2, sg_last_cnt = -1, sg_last_act = -1;

static lv_obj_t *sg_chart, *sg_g_title, *sg_g_info, *sg_g_ymax, *sg_g_ymin, *sg_g_mode_lbl;
static lv_chart_series_t *sg_ser_act, *sg_ser_prj;
static int       sg_g_ch = 0;
static fs_hist_t sg_g_hist = FS_HIST_SHORT;

static uint32_t sev_color(int sev)
{
    switch (sev) {
    case FS_SEV_INFO:    return 0x1976D2;
    case FS_SEV_CAUTION: return 0xF9A825;
    case FS_SEV_WARNING: return 0xD32F2F;
    default:             return 0x6A1B9A;
    }
}

static void fmt_value(char *buf, size_t n, float v, uint8_t dec)
{
    int neg = (v < 0);
    if (neg) v = -v;
    int scale = (dec == 0) ? 1 : (dec == 1) ? 10 : 100;
    int32_t sc = (int32_t)(v * scale + 0.5f);
    int ip = (int)(sc / scale), fp = (int)(sc % scale);
    if (dec == 0)      snprintf(buf, n, "%s%d", neg ? "-" : "", ip);
    else if (dec == 1) snprintf(buf, n, "%s%d.%d", neg ? "-" : "", ip, fp);
    else               snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", ip, fp);
}

/* ------------------------------------------------------------------ graph */
static void graph_refresh(void)
{
    int cnt = fs_count();
    if (cnt <= 0) return;
    if (sg_g_ch >= cnt) sg_g_ch = 0;
    int ch = sg_g_ch;
    const fs_sensor_t *d = fs_def(ch);

    float hist[FS_H_LEN];
    float fc[PROJ_STEPS];
    int n = fs_hist_get(ch, sg_g_hist, hist, FS_H_LEN);
    int m = fs_holt(hist, n, PROJ_STEPS, fc);
    for (int k = 0; k < m; k++) {
        if (fc[k] < d->min_valid) fc[k] = d->min_valid;
        if (fc[k] > d->max_valid) fc[k] = d->max_valid;
    }

    /* y range from the data, padded */
    float lo = d->min_valid, hi = d->max_valid;
    if (n > 0) {
        lo = hist[0];
        hi = hist[0];
        for (int i = 1; i < n; i++) {
            if (hist[i] < lo) lo = hist[i];
            if (hist[i] > hi) hi = hist[i];
        }
        for (int k = 0; k < m; k++) {
            if (fc[k] < lo) lo = fc[k];
            if (fc[k] > hi) hi = fc[k];
        }
    }
    float minspan = (d->max_valid - d->min_valid) * 0.02f;
    if ((hi - lo) < minspan) {
        float mid = (hi + lo) * 0.5f;
        lo = mid - minspan * 0.5f;
        hi = mid + minspan * 0.5f;
    }
    float pad = (hi - lo) * 0.1f;
    lo -= pad;
    hi += pad;
    int32_t ilo = (int32_t)(lo * 100.0f);
    int32_t ihi = (int32_t)(hi * 100.0f);
    if (ihi <= ilo) ihi = ilo + 1;
    FS_CHART_RANGE(sg_chart, ilo, ihi);

    /* push exactly CH_PTS values per series so the buffer is fully rewritten */
    for (int i = 0; i < CH_PTS; i++) {
        int32_t v = (i < n) ? (int32_t)(hist[i] * 100.0f) : (int32_t)LV_CHART_POINT_NONE;
        lv_chart_set_next_value(sg_chart, sg_ser_act, v);
    }
    for (int i = 0; i < CH_PTS; i++) {
        int32_t v = (int32_t)LV_CHART_POINT_NONE;
        if (m > 0 && n > 0) {
            if (i == n - 1)                v = (int32_t)(hist[n - 1] * 100.0f);
            else if (i >= n && (i - n) < m) v = (int32_t)(fc[i - n] * 100.0f);
        }
        lv_chart_set_next_value(sg_chart, sg_ser_prj, v);
    }

    char a[24], b[24];
    fmt_value(a, sizeof(a), hi, d->decimals);
    fmt_value(b, sizeof(b), lo, d->decimals);
    lv_label_set_text(sg_g_ymax, a);
    lv_label_set_text(sg_g_ymin, b);

    /* title with the current value */
    fs_sample_t s;
    if (fs_get(ch, &s) && s.quality != FS_Q_INVALID) {
        fmt_value(a, sizeof(a), s.value, d->decimals);
        lv_label_set_text_fmt(sg_g_title, "%s: %s %s", d->label, a, d->unit);
    } else {
        lv_label_set_text_fmt(sg_g_title, "%s: --", d->label);
    }

    /* info line: window length and projected threshold crossing */
    uint32_t step_s = fs_hist_period_ms(sg_g_hist) / 1000u;
    int win_min = (int)((step_s * FS_H_LEN) / 60u);
    char info[112];

    if (m == 0) {
        snprintf(info, sizeof(info), "Window %d min | collecting data for projection...", win_min);
    } else {
        int best_k = 0;
        const char *best_txt = NULL;
        float last = hist[n - 1];
        for (int r = 0; r < d->n_rules; r++) {
            const fs_rule_def_t *rd = &d->rules[r];
            bool below = (rd->cmp == FS_CMP_BELOW);
            bool already = below ? (last < rd->threshold) : (last > rd->threshold);
            if (already) continue;
            int k = fs_cross_step(fc, m, rd->threshold, below);
            if (k > 0 && (best_k == 0 || k < best_k)) { best_k = k; best_txt = rd->text; }
        }
        if (best_k > 0) {
            int mins = (int)((uint32_t)best_k * step_s + 30u) / 60;
            snprintf(info, sizeof(info), "Window %d min | %s in ~%d min", win_min, best_txt, mins);
        } else {
            snprintf(info, sizeof(info), "Window %d min | no threshold crossing projected", win_min);
        }
    }
    lv_label_set_text(sg_g_info, info);
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    int n = fs_count();
    if (n <= 0) return;
    sg_g_ch = (sg_g_ch + n - 1) % n;
    graph_refresh();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    int n = fs_count();
    if (n <= 0) return;
    sg_g_ch = (sg_g_ch + 1) % n;
    graph_refresh();
}

static void mode_cb(lv_event_t *e)
{
    (void)e;
    sg_g_hist = (sg_g_hist == FS_HIST_SHORT) ? FS_HIST_LONG : FS_HIST_SHORT;
    lv_label_set_text(sg_g_mode_lbl, (sg_g_hist == FS_HIST_SHORT) ? "LIVE" : "LONG");
    graph_refresh();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 34);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void build_graph_tile(lv_obj_t *tile)
{
    lv_obj_t *b;

    b = make_btn(tile, "<", prev_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, BANNER_H + 4);
    b = make_btn(tile, ">", next_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 68, BANNER_H + 4);
    b = make_btn(tile, "LIVE", mode_cb, &sg_g_mode_lbl);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, BANNER_H + 4);

    sg_g_title = lv_label_create(tile);
    lv_label_set_text(sg_g_title, "--");
    lv_obj_set_style_text_color(sg_g_title, lv_color_hex(0x222222), 0);
    lv_obj_align(sg_g_title, LV_ALIGN_TOP_LEFT, 136, BANNER_H + 12);

    sg_chart = lv_chart_create(tile);
    lv_obj_set_size(sg_chart, lv_pct(92), 170);
    lv_obj_align(sg_chart, LV_ALIGN_TOP_MID, 0, BANNER_H + 44);
    lv_chart_set_type(sg_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(sg_chart, CH_PTS);
    lv_chart_set_div_line_count(sg_chart, 4, 6);
    lv_obj_set_style_width(sg_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(sg_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sg_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(sg_chart, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(sg_chart, 150, 0);
    lv_obj_set_style_border_width(sg_chart, 0, 0);
    lv_obj_set_style_radius(sg_chart, 12, 0);
    sg_ser_act = lv_chart_add_series(sg_chart, lv_color_hex(0x2E7D32), LV_CHART_AXIS_PRIMARY_Y);
    sg_ser_prj = lv_chart_add_series(sg_chart, lv_color_hex(0xEF6C00), LV_CHART_AXIS_PRIMARY_Y);

    sg_g_ymax = lv_label_create(sg_chart);
    lv_label_set_text(sg_g_ymax, "");
    lv_obj_align(sg_g_ymax, LV_ALIGN_TOP_LEFT, 6, 2);
    sg_g_ymin = lv_label_create(sg_chart);
    lv_label_set_text(sg_g_ymin, "");
    lv_obj_align(sg_g_ymin, LV_ALIGN_BOTTOM_LEFT, 6, -2);

    sg_g_info = lv_label_create(tile);
    lv_label_set_text(sg_g_info, "");
    lv_obj_set_style_text_color(sg_g_info, lv_color_hex(0x222222), 0);
    lv_obj_align(sg_g_info, LV_ALIGN_TOP_MID, 0, BANNER_H + 222);
}

/* -------------------------------------------------------------- dashboard */
static lv_obj_t *make_panel(lv_obj_t *parent, const char *title, lv_obj_t **val_out)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 140, 80);
    lv_obj_set_style_bg_color(p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(p, 150, 0);
    lv_obj_set_style_border_width(p, 4, 0);
    lv_obj_set_style_border_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0x333333), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *v = lv_label_create(p);
    lv_label_set_text(v, "--");
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 8);
    *val_out = v;
    return p;
}

static void panel_click_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    sg_g_ch = ch;
    graph_refresh();
    lv_tileview_set_tile(sg_tv, sg_tile_graph, LV_ANIM_ON);
}

static void leaf_anim_cb(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }

static void create_falling_leaf(lv_obj_t *parent, int x, int delay_ms)
{
    lv_obj_t *leaf = lv_image_create(parent);
    lv_image_set_src(leaf, &leaf_sprite);
    lv_obj_set_pos(leaf, x, -20);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, leaf);
    lv_anim_set_exec_cb(&a, leaf_anim_cb);
    lv_anim_set_values(&a, -20, 340);
    lv_anim_set_duration(&a, 4000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_start(&a);
}

/* ----------------------------------------------------------------- banner */
static void update_banner(void)
{
    int top = fs_alarm_top();
    int cnt = fs_alarm_shown_count();
    fs_alarm_info_t a;
    int act = -1;

    if (top >= 0 && fs_alarm_get(top, &a)) act = a.active ? 1 : 0;
    if (top == sg_last_top && cnt == sg_last_cnt && act == sg_last_act) return;
    sg_last_top = top;
    sg_last_cnt = cnt;
    sg_last_act = act;

    if (act < 0) {
        lv_obj_set_style_bg_color(sg_banner, lv_color_hex(0x2E7D32), 0);
        lv_obj_set_style_text_color(sg_banner_lbl, lv_color_white(), 0);
        lv_label_set_text(sg_banner_lbl, "ALL SYSTEMS NOMINAL");
        return;
    }

    lv_obj_set_style_bg_color(sg_banner, lv_color_hex(sev_color((int)a.rule->sev)), 0);
    lv_obj_set_style_text_color(sg_banner_lbl,
        (a.rule->sev == FS_SEV_CAUTION) ? lv_color_hex(0x212121) : lv_color_white(), 0);

    if (!a.active) {
        lv_label_set_text_fmt(sg_banner_lbl, "[%u] %s (cleared, tap to ack)",
                              (unsigned)a.rule->code, a.rule->text);
    } else if (cnt > 1) {
        lv_label_set_text_fmt(sg_banner_lbl, "[%u] %s  (+%d)",
                              (unsigned)a.rule->code, a.rule->text, cnt - 1);
    } else {
        lv_label_set_text_fmt(sg_banner_lbl, "[%u] %s",
                              (unsigned)a.rule->code, a.rule->text);
    }
}

static void banner_tap_cb(lv_event_t *e)
{
    (void)e;
    fs_alarm_ack();
    sg_last_top = -2;
}

/* runs inside LVGL's task: the only place widgets are touched at runtime */
static void refresh_cb(lv_timer_t *t)
{
    static int tick = 0;
    (void)t;

    for (int i = 0; i < fs_count() && i < UI_MAX; i++) {
        const fs_sensor_t *d = fs_def(i);
        fs_sample_t s;
        char num[16], buf[32];

        if (!fs_get(i, &s)) {
            lv_label_set_text(sg_val[i], "--");
            lv_obj_set_style_text_color(sg_val[i], lv_color_hex(0x999999), 0);
        } else if (s.quality == FS_Q_INVALID) {
            lv_label_set_text(sg_val[i], "ERR");
            lv_obj_set_style_text_color(sg_val[i], lv_color_hex(0xD32F2F), 0);
        } else {
            fmt_value(num, sizeof(num), s.value, d->decimals);
            snprintf(buf, sizeof(buf), "%s %s%s", num, d->unit,
                     (s.quality == FS_Q_STALE) ? " ?" : "");
            lv_label_set_text(sg_val[i], buf);
            lv_obj_set_style_text_color(sg_val[i],
                lv_color_hex(s.quality == FS_Q_STALE ? 0x999999 : 0x333333), 0);
        }

        int sev = fs_alarm_channel_sev(i);
        if (sev >= 0) {
            lv_obj_set_style_border_color(sg_panel[i], lv_color_hex(sev_color(sev)), 0);
            lv_obj_set_style_border_opa(sg_panel[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_opa(sg_panel[i], LV_OPA_TRANSP, 0);
        }
    }
    update_banner();
    fs_ui_insight_refresh();

    /* redraw the graph about once a second, and only while it is on screen */
    tick++;
    if ((tick & 1) == 0 && lv_obj_get_scroll_x(sg_tv) < lv_obj_get_width(sg_tv)) {
        graph_refresh();
    }
}

void fs_ui_build(void)
{
    lv_obj_t *bg = lv_image_create(lv_screen_active());
    lv_image_set_src(bg, &ghibli_bg);

    create_falling_leaf(lv_screen_active(), 50, 0);
    create_falling_leaf(lv_screen_active(), 150, 1500);
    create_falling_leaf(lv_screen_active(), 250, 3000);

    /* tile view: graph on the left, dashboard on the right (swipe right = graph) */
    sg_tv = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(sg_tv, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(sg_tv, 0, 0);
    lv_obj_set_style_bg_opa(sg_tv, LV_OPA_TRANSP, 0);
    lv_obj_update_layout(sg_tv);

    sg_tile_graph = lv_tileview_add_tile(sg_tv, 0, 0, LV_DIR_RIGHT);
    sg_tile_dash  = lv_tileview_add_tile(sg_tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    sg_tile_insight = lv_tileview_add_tile(sg_tv, 2, 0, LV_DIR_LEFT);
    lv_obj_set_style_bg_opa(sg_tile_insight, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(sg_tile_graph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(sg_tile_dash, LV_OPA_TRANSP, 0);

    /* dashboard: panels come from the sensor table */
    lv_obj_t *cont = lv_obj_create(sg_tile_dash);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_top(cont, BANNER_H + 8, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_style_pad_column(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    for (int i = 0; i < fs_count() && i < UI_MAX; i++) {
        sg_panel[i] = make_panel(cont, fs_def(i)->label, &sg_val[i]);
        lv_obj_add_event_cb(sg_panel[i], panel_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    build_graph_tile(sg_tile_graph);
    fs_ui_insight_build(sg_tile_insight, BANNER_H);

    /* alarm banner on top of everything */
    sg_banner = lv_obj_create(lv_screen_active());
    lv_obj_set_size(sg_banner, lv_pct(100), BANNER_H);
    lv_obj_set_pos(sg_banner, 0, 0);
    lv_obj_set_style_radius(sg_banner, 0, 0);
    lv_obj_set_style_border_width(sg_banner, 0, 0);
    lv_obj_set_style_pad_all(sg_banner, 0, 0);
    lv_obj_set_style_bg_opa(sg_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sg_banner, lv_color_hex(0x2E7D32), 0);
    lv_obj_remove_flag(sg_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sg_banner, banner_tap_cb, LV_EVENT_CLICKED, NULL);

    sg_banner_lbl = lv_label_create(sg_banner);
    lv_label_set_text(sg_banner_lbl, "ALL SYSTEMS NOMINAL");
    lv_obj_set_style_text_color(sg_banner_lbl, lv_color_white(), 0);
    lv_obj_center(sg_banner_lbl);

    lv_obj_update_layout(sg_tv);
    lv_tileview_set_tile(sg_tv, sg_tile_dash, LV_ANIM_OFF);

    lv_timer_create(refresh_cb, 500, NULL);
}
