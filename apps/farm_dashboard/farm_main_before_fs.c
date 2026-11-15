#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"

LV_IMAGE_DECLARE(ghibli_bg);
LV_IMAGE_DECLARE(leaf_sprite);

static lv_obj_t *local_moisture_label;
static lv_obj_t *nasa_smap_label;

static lv_obj_t *create_data_panel(lv_obj_t *parent, int x, int y,
                                   int w, int h, const char *title_text)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, 150, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, title_text);
#if defined(LV_FONT_MONTSERRAT_16) && (LV_FONT_MONTSERRAT_16 == 1)
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *data_label = lv_label_create(panel);
    lv_label_set_text(data_label, "--");
    lv_obj_align(data_label, LV_ALIGN_CENTER, 0, 10);
    return data_label;
}

static void leaf_anim_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void create_falling_leaf(lv_obj_t *parent, int start_x, int delay_ms)
{
    lv_obj_t *leaf = lv_image_create(parent);
    lv_image_set_src(leaf, &leaf_sprite);
    lv_obj_set_pos(leaf, start_x, -20);

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

/* runs inside LVGL's own task, so touching widgets here is safe */
static void sensor_timer_cb(lv_timer_t *t)
{
    (void)t;
    float local_moisture_pct = 42.5f;   /* placeholder until a real sensor is wired */
    lv_label_set_text_fmt(local_moisture_label, "%d.%d %%",
                          (int)local_moisture_pct,
                          (int)(local_moisture_pct * 10) % 10);
}

static void setup_dashboard(void)
{
    lv_obj_t *bg = lv_image_create(lv_screen_active());
    lv_image_set_src(bg, &ghibli_bg);

    create_falling_leaf(lv_screen_active(), 50, 0);
    create_falling_leaf(lv_screen_active(), 150, 1500);
    create_falling_leaf(lv_screen_active(), 250, 3000);

    local_moisture_label = create_data_panel(lv_screen_active(), 20, 20, 130, 80, "Local Soil");
    nasa_smap_label      = create_data_panel(lv_screen_active(), 170, 20, 130, 80, "NASA SMAP");

    lv_timer_create(sensor_timer_cb, 1000, NULL);
}

void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("farm_dashboard | board: %s", PLATFORM_BOARD);

    board_register_hardware();
    lv_vendor_init(DISPLAY_NAME);
    setup_dashboard();
    lv_vendor_start(5, 1024 * 8);
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[]) { user_main(); while (1) tal_system_sleep(500); }
#else
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif