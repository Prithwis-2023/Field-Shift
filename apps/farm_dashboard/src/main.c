#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"
#include "fs_core.h"
#include "fs_alarm.h"
#include "fs_ui.h"

void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("field framework | board: %s", PLATFORM_BOARD);

    board_register_hardware();

    int n = 0;
    const fs_sensor_t *sensors = fs_profile_sensors(&n);
    if (fs_init(sensors, n) != 0) {
        PR_ERR("fs_init failed");
        return;
    }
    fs_alarm_start();
    fs_start();
    fs_profile_start();

    lv_vendor_init(DISPLAY_NAME);
    fs_ui_build();
    lv_vendor_start(5, 1024 * 8);

    while (1) {
        PR_NOTICE("free heap: %d bytes", tal_system_get_free_heap_size());
        tal_system_sleep(30000);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[]) { user_main(); }
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
