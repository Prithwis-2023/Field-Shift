#include "fs_ui_insight.h"
#include "fs_core.h"
#include "fs_insight.h"
#include "fs_time.h"
#include <stdio.h>

static lv_obj_t *sg_verdict, *sg_reason, *sg_bar, *sg_text, *sg_src;

static void fmt_ch(char *b, size_t n, int ch, const char *unit, int dec)
{
    fs_sample_t s;
    if (ch < 0 || !fs_get(ch, &s) || s.quality == FS_Q_INVALID) { snprintf(b, n, "--"); return; }
    float v = s.value;
    int neg = (v < 0.0f);
    if (neg) v = -v;
    int sc = (dec == 0) ? 1 : (dec == 1) ? 10 : 100;
    int32_t c = (int32_t)(v * (float)sc + 0.5f);
    const char *sg = neg ? "-" : "";
    const char *q = (s.quality == FS_Q_STALE) ? "?" : "";
    if (dec == 0)      snprintf(b, n, "%s%d%s%s", sg, (int)c, unit, q);
    else if (dec == 1) snprintf(b, n, "%s%d.%d%s%s", sg, (int)(c / 10), (int)(c % 10), unit, q);
    else               snprintf(b, n, "%s%d.%02d%s%s", sg, (int)(c / 100), (int)(c % 100), unit, q);
}

void fs_ui_insight_build(lv_obj_t *tile, int top_offset)
{
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, lv_pct(94), 278);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top_offset + 2);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(card, 170, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "FIELD INSIGHT: LOCAL vs SATELLITE");
    lv_obj_set_style_text_color(title, lv_color_hex(0x444444), 0);

    sg_verdict = lv_label_create(card);
    lv_label_set_text(sg_verdict, "WAITING FOR DATA");
#if defined(LV_FONT_MONTSERRAT_24) && (LV_FONT_MONTSERRAT_24 == 1)
    lv_obj_set_style_text_font(sg_verdict, &lv_font_montserrat_24, 0);
#endif

    sg_reason = lv_label_create(card);
    lv_label_set_long_mode(sg_reason, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sg_reason, lv_pct(100));
    lv_label_set_text(sg_reason, "");
    lv_obj_set_style_text_color(sg_reason, lv_color_hex(0x333333), 0);

    sg_bar = lv_bar_create(card);
    lv_obj_set_size(sg_bar, lv_pct(100), 14);
    lv_bar_set_range(sg_bar, 0, 100);
    lv_bar_set_value(sg_bar, 0, LV_ANIM_OFF);

    sg_text = lv_label_create(card);
    lv_label_set_long_mode(sg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sg_text, lv_pct(100));
    lv_label_set_text(sg_text, "");
    lv_obj_set_style_text_color(sg_text, lv_color_hex(0x222222), 0);

    sg_src = lv_label_create(card);
    lv_label_set_long_mode(sg_src, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sg_src, lv_pct(100));
    lv_label_set_text(sg_src, "");
    lv_obj_set_style_text_color(sg_src, lv_color_hex(0xB26A00), 0);
}

void fs_ui_insight_refresh(void)
{
    static int c_soil = -2, c_macro, c_root, c_rain, c_rain7, c_rtmin, c_gap, c_tgap, c_corr, c_irr;
    char reason[80], txt[300], clk[16];
    char a[20], b[20], c[20], g[20], r[20], tg[20], r7[20], r1[20], tm[20];
    fs_sample_t s;
    int irr = 0, h = 0, m = 0;

    if (c_soil == -2) {
        c_soil  = fs_find("soil");
        c_macro = fs_find("macro");
        c_root  = fs_find("root");
        c_rain  = fs_find("rain");
        c_rain7 = fs_find("rain7");
        c_rtmin = fs_find("rtmin");
        c_gap   = fs_find("gap");
        c_tgap  = fs_find("tgap");
        c_corr  = fs_find("corr");
        c_irr   = fs_find("irr");
    }

    int level = ins_verdict(reason, (int)sizeof(reason));
    uint32_t col = (level == 3) ? 0xD32F2F : (level == 2) ? 0xF9A825 :
                   (level == 1) ? 0x2E7D32 : 0x777777;

    lv_label_set_text(sg_verdict, ins_level_text(level));
    lv_obj_set_style_text_color(sg_verdict, lv_color_hex(col), 0);
    lv_label_set_text(sg_reason, reason);

    if (c_irr >= 0 && fs_get(c_irr, &s) && s.quality != FS_Q_INVALID) irr = (int)s.value;
    lv_bar_set_value(sg_bar, irr, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sg_bar, lv_color_hex(col), LV_PART_INDICATOR);

    fmt_ch(a,  sizeof(a),  c_soil,  "%",    0);
    fmt_ch(b,  sizeof(b),  c_macro, "%",    0);
    fmt_ch(c,  sizeof(c),  c_root,  "%",    0);
    fmt_ch(g,  sizeof(g),  c_gap,   " pts", 0);
    fmt_ch(r,  sizeof(r),  c_corr,  "",     2);
    fmt_ch(tg, sizeof(tg), c_tgap,  " C",   1);
    fmt_ch(r7, sizeof(r7), c_rain7, " mm",  1);
    fmt_ch(r1, sizeof(r1), c_rain,  " mm",  1);
    fmt_ch(tm, sizeof(tm), c_rtmin, " C",   1);

    snprintf(txt, sizeof(txt),
             "Local soil: %s   Regional surface: %s   Root zone: %s\n"
             "Gap: %s   Correlation r: %s (%d pairs)   Temp gap: %s\n"
             "Rain 7 d: %s   Last day: %s   Regional min T: %s",
             a, b, c, g, r, ins_pair_count(), tg, r7, r1, tm);
    lv_label_set_text(sg_text, txt);

    if (fs_time_hm(&h, &m)) snprintf(clk, sizeof(clk), "%02d:%02d", h, m);
    else                    snprintf(clk, sizeof(clk), "--:--");
    lv_label_set_text_fmt(sg_src, "Local sensors: %s    UTC %s\nRegional: %s",
                          (fs_sim_count() > 0) ? "SIMULATED" : "measured", clk, ins_source());
}
