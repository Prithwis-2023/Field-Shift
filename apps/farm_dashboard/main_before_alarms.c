#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"
#include "fs_core.h"
#include <stdio.h>

LV_IMAGE_DECLARE(ghibli_bg);
LV_IMAGE_DECLARE(leaf_sprite);

#define UI_MAX 16
static lv_obj_t *sg_val[UI_MAX];

static void fmt_value(char *buf, size_t n, float v, uint8_t dec)
{
    int neg = (v < 0);
    if (neg)
        v = -v;
    int     scale = (dec == 0) ? 1 : (dec == 1) ? 10 : 100;
    int32_t sc    = (int32_t)(v * scale + 0.5f);
    int     ip = (int)(sc / scale), fp = (int)(sc % scale);
    if (dec == 0)
        snprintf(buf, n, "%s%d", neg ? "-" : "", ip);
    else if (dec == 1)
        snprintf(buf, n, "%s%d.%d", neg ? "-" : "", ip, fp);
    else
        snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", ip, fp);
}

static lv_obj_t *make_panel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 140, 80);
    lv_obj_set_style_bg_color(p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(p, 150, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0x333333), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *v = lv_label_create(p);
    lv_label_set_text(v, "--");
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 10);
    return v;
}

static void leaf_anim_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

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

/* runs inside LVGL's task: the only place widgets are touched at runtime */
static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < fs_count() && i < UI_MAX; i++) {
        const fs_channel_def_t *d = fs_def(i);
        fs_sample_t             s;
        char                    num[16], buf[32];

        if (!fs_get(i, &s)) {
            lv_label_set_text(sg_val[i], "--");
            lv_obj_set_style_text_color(sg_val[i], lv_color_hex(0x999999), 0);
        } else if (s.quality == FS_Q_INVALID) {
            lv_label_set_text(sg_val[i], "ERR");
            lv_obj_set_style_text_color(sg_val[i], lv_color_hex(0xD32F2F), 0);
        } else {
            fmt_value(num, sizeof(num), s.value, d->decimals);
            snprintf(buf, sizeof(buf), "%s %s%s", num, d->unit, (s.quality == FS_Q_STALE) ? " ?" : "");
            lv_label_set_text(sg_val[i], buf);
            lv_obj_set_style_text_color(sg_val[i], lv_color_hex(s.quality == FS_Q_STALE ? 0x999999 : 0x333333), 0);
        }
    }
}

static void setup_dashboard(void)
{
    lv_obj_t *bg = lv_image_create(lv_screen_active());
    lv_image_set_src(bg, &ghibli_bg);

    create_falling_leaf(lv_screen_active(), 50, 0);
    create_falling_leaf(lv_screen_active(), 150, 1500);
    create_falling_leaf(lv_screen_active(), 250, 3000);

    lv_obj_t *cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_style_pad_column(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < fs_count() && i < UI_MAX; i++) {
        sg_val[i] = make_panel(cont, fs_def(i)->label);
    }
    lv_timer_create(refresh_cb, 500, NULL);
}

void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("farm_dashboard (channel framework) | board: %s", PLATFORM_BOARD);

    board_register_hardware();

    if (fs_init(fs_profile_defs(), fs_profile_count()) != 0) {
        PR_ERR("fs_init failed");
        return;
    }
    fs_profile_start();

    lv_vendor_init(DISPLAY_NAME);
    setup_dashboard();
    lv_vendor_start(5, 1024 * 8);
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
    while (1)
        tal_system_sleep(500);
}
#else
static THREAD_HANDLE ty_app_thread = NULL;
static void          tuya_app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}
void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth   = 1024 * 4;
    thrd_param.priority     = THREAD_PRIO_1;
    thrd_param.thrdname     = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif