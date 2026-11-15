#include "fs_core.h"
#include "fs_insight.h"
#include "fs_net.h"

/* 1 = real NASA POWER feed, 0 = simulated regional data */
#define USE_LIVE_MACRO 1
#if USE_LIVE_MACRO
#define MACRO_TIMING .max_age_ms = 28800000u, .lost_ms = 86400000u, .poll_ms = 0,
#define MACRO_READ   NULL
#define RAIN_READ    NULL
#define CORR_LOST_MS 86400000u
#else
#define MACRO_TIMING .max_age_ms = 90000, .lost_ms = 300000, .poll_ms = 20000,
#define MACRO_READ   mock_macro
#define RAIN_READ    mock_rain
#define CORR_LOST_MS 300000
#endif
#include "tal_api.h"

#define N_OF(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static uint32_t secs(void) { return (uint32_t)(tal_system_get_millisecond() / 1000); }

static float tri(uint32_t t_s, uint32_t period)
{
    uint32_t p = t_s % period;
    float x = (float)p / (float)(period / 2);
    return (x <= 1.0f) ? x : (2.0f - x);
}

/* ---- simulated LOCAL sensors: replace each read() with a real driver ---- */
static int read_soil(float *raw)
{
    uint32_t t = secs();
    uint32_t c = t % 300;
    if ((c >= 60 && c < 75) || (c >= 150 && c < 225)) return -1;
    if (c == 80) { *raw = -999.0f; return 0; }
    *raw = 3000.0f - 20.0f * (15.0f + 50.0f * tri(t, 240));   /* raw ADC counts */
    return 0;
}
static int read_airt(float *raw) { *raw = -4.0f + 44.0f * tri(secs(), 600); return 0; }
static int read_hum(float *raw)  { *raw = 40.0f + 40.0f * tri(secs(), 150); return 0; }
static int read_lux(float *raw)  { *raw = 60000.0f * tri(secs(), 240);      return 0; }

/* ---- SIMULATED REGIONAL feed: the network task replaces these two later ---- */
static int __attribute__((unused)) mock_macro(float *raw)
{
    /* tracks the same wetness curve with a lag and an offset, so correlation is high */
    *raw = 30.0f + 0.6f * (15.0f + 50.0f * tri(secs() + 210u, 240));
    return 0;
}
static int __attribute__((unused)) mock_rain(float *raw)
{
    *raw = ((secs() % 900) < 300) ? 12.0f : 0.0f;
    return 0;
}

/* ---- rules, attached per sensor row ---- */
static const fs_rule_def_t soil_rules[] = {
    { 1001, "IRRIGATE: SOIL LOW", FS_CMP_BELOW, 30.0f, 3.0f, 30000, FS_SEV_CAUTION, false },
};
static const fs_rule_def_t airt_rules[] = {
    { 1002, "FROST RISK",  FS_CMP_BELOW,  2.0f, 1.0f, 30000, FS_SEV_WARNING, true  },
    { 1003, "HEAT STRESS", FS_CMP_ABOVE, 35.0f, 1.0f, 60000, FS_SEV_CAUTION, false },
};
static const fs_rule_def_t gap_rules[] = {
    { 3001, "LOCAL DRIER THAN REGION", FS_CMP_BELOW, -20.0f, 3.0f, 60000, FS_SEV_CAUTION, false },
};
static const fs_rule_def_t irr_rules[] = {
    { 3002, "IRRIGATION NEED HIGH", FS_CMP_ABOVE, 50.0f, 5.0f, 15000, FS_SEV_CAUTION, false },
};

/* ---- the sensor table: one row per channel (local, regional or derived) ---- */
static const fs_sensor_t sg_sensors[] = {
    { .key = "soil", .label = "Local Soil", .unit = "%",
      .min_valid = 0.0f, .max_valid = 100.0f, .decimals = 1,
      .cal_scale  = FS_CAL2_SCALE(3000.0f, 0.0f, 1000.0f, 100.0f),
      .cal_offset = FS_CAL2_OFFSET(3000.0f, 0.0f, 1000.0f, 100.0f),
      .max_age_ms = 10000, .poll_ms = 500, .read = read_soil,
      .rules = soil_rules, .n_rules = N_OF(soil_rules) },

    { .key = "airt", .label = "Air Temp", .unit = "C",
      .min_valid = -40.0f, .max_valid = 80.0f, .decimals = 1,
      .max_age_ms = 10000, .poll_ms = 1000, .read = read_airt,
      .rules = airt_rules, .n_rules = N_OF(airt_rules) },

    { .key = "hum", .label = "Humidity", .unit = "%",
      .min_valid = 0.0f, .max_valid = 100.0f, .decimals = 0,
      .max_age_ms = 10000, .poll_ms = 1000, .read = read_hum },

    { .key = "lux", .label = "Light", .unit = "lx",
      .min_valid = 0.0f, .max_valid = 120000.0f, .decimals = 0,
      .max_age_ms = 10000, .poll_ms = 1000, .read = read_lux },

    /* regional (mock for now) */
    { .key = "macro", .label = "Regional Soil", .unit = "%",
      .min_valid = 0.0f, .max_valid = 100.0f, .decimals = 0,
      MACRO_TIMING .read = MACRO_READ },

    { .key = "rain", .label = "Rain/day", .unit = "mm",
      .min_valid = 0.0f, .max_valid = 200.0f, .decimals = 0,
      MACRO_TIMING .read = RAIN_READ },

    /* derived: computed from other channels by fs_insight.c */
    { .key = "gap", .label = "Soil Gap", .unit = "pts",
      .min_valid = -100.0f, .max_valid = 100.0f, .decimals = 0,
      .max_age_ms = 20000, .lost_ms = 300000, .poll_ms = 5000, .read = ins_read_gap,
      .rules = gap_rules, .n_rules = N_OF(gap_rules) },

    { .key = "corr", .label = "Correlation", .unit = "r",
      .min_valid = -1.0f, .max_valid = 1.0f, .decimals = 2,
      .max_age_ms = 30000, .lost_ms = CORR_LOST_MS, .poll_ms = 5000, .read = ins_read_corr },

    { .key = "irr", .label = "Irrigation Need", .unit = "/100",
      .min_valid = 0.0f, .max_valid = 100.0f, .decimals = 0,
      .max_age_ms = 20000, .lost_ms = 300000, .poll_ms = 5000, .read = ins_read_irr,
      .rules = irr_rules, .n_rules = N_OF(irr_rules) },
};

const fs_sensor_t *fs_profile_sensors(int *count)
{
    *count = (int)(sizeof(sg_sensors) / sizeof(sg_sensors[0]));
    return sg_sensors;
}

void fs_profile_start(void)
{
#if USE_LIVE_MACRO
    fs_net_start();
#endif
}
