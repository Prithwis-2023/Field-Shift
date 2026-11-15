#include "fs_core.h"
#include "tal_api.h"

enum { CH_SOIL, CH_AIR_T, CH_HUM, CH_LUX, CH_COUNT };

static const fs_channel_def_t sg_defs[CH_COUNT] = {
    /* key,   label,        unit, min,  max,    max_age, dec, hist_period */
    { "soil", "Local Soil", "%",  0,    100,    10000,   1,   5000 },
    { "airt", "Air Temp",   "C",  -40,  80,     10000,   1,   5000 },
    { "hum",  "Humidity",   "%",  0,    100,    10000,   0,   5000 },
    { "lux",  "Light",      "lx", 0,    120000, 10000,   0,   5000 },
};

const fs_channel_def_t *fs_profile_defs(void)  { return sg_defs; }
int                     fs_profile_count(void) { return CH_COUNT; }

/* triangle wave 0..1 */
static float tri(uint32_t t_s, uint32_t period)
{
    uint32_t p = t_s % period;
    float x = (float)p / (float)(period / 2);
    return (x <= 1.0f) ? x : (2.0f - x);
}

static void sim_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t t = (uint32_t)(tal_system_get_millisecond() / 1000);
        uint32_t cycle = t % 90;

        /* fault injection to exercise the quality flags:
           soil goes silent 60-75 s (STALE), and one bad reading at 80 s (INVALID) */
        if (cycle == 80)                     fs_publish(CH_SOIL, -999.0f);
        else if (cycle < 60 || cycle >= 75)  fs_publish(CH_SOIL, 30.0f + 40.0f * tri(t, 120));

        fs_publish(CH_AIR_T, 18.0f + 10.0f * tri(t, 180));
        fs_publish(CH_HUM,   40.0f + 40.0f * tri(t, 150));
        fs_publish(CH_LUX,   60000.0f * tri(t, 240));
        tal_system_sleep(1000);
    }
}

void fs_profile_start(void)
{
    THREAD_HANDLE h = NULL;
    THREAD_CFG_T cfg = {0};
    cfg.stackDepth = 1024 * 4;
    cfg.priority   = THREAD_PRIO_2;
    cfg.thrdname   = "fs_sim";
    tal_thread_create_and_start(&h, NULL, NULL, sim_task, NULL, &cfg);
}
