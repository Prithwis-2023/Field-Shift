#ifndef FS_CORE_H
#define FS_CORE_H
#include <stdint.h>
#include <stdbool.h>

#define FS_MAX_CHANNELS    16
#define FS_H_LEN           96       /* points per history buffer                */
#define FS_HS_PERIOD_MS    5000     /* short history: 96 x 5 s  = 8 min         */
#define FS_HL_PERIOD_MS    60000    /* long history:  96 x 60 s = 96 min        */
#define FS_DEFAULT_LOST_MS 60000

typedef enum { FS_Q_NO_DATA = 0, FS_Q_OK, FS_Q_STALE, FS_Q_INVALID } fs_quality_t;
typedef enum { FS_SEV_INFO = 0, FS_SEV_CAUTION, FS_SEV_WARNING, FS_SEV_FAILURE } fs_sev_t;
typedef enum { FS_CMP_BELOW = 0, FS_CMP_ABOVE } fs_cmp_t;
typedef enum { FS_HIST_SHORT = 0, FS_HIST_LONG } fs_hist_t;

/* a threshold rule attached to a sensor row */
typedef struct {
    uint16_t    code;
    const char *text;
    fs_cmp_t    cmp;
    float       threshold;
    float       hysteresis;
    uint32_t    hold_ms;
    fs_sev_t    sev;
    bool        latching;
} fs_rule_def_t;

/* one row per sensor: everything the framework needs to know about it */
typedef struct {
    const char *key;
    const char *label;
    const char *unit;
    float       min_valid;
    float       max_valid;
    uint8_t     decimals;
    float       cal_scale;       /* value = raw * scale + offset; scale 0 => 1.0 */
    float       cal_offset;
    uint32_t    max_age_ms;      /* older than this => STALE (0 = never)         */
    uint32_t    lost_ms;         /* "sensor lost" alarm delay (0 => default)     */
    uint32_t    poll_ms;         /* 0 => someone else calls fs_publish()         */
    int       (*read)(float *raw);       /* returns 0 on success                 */
    const fs_rule_def_t *rules;
    uint8_t     n_rules;
} fs_sensor_t;

/* two-point calibration helpers for static initialisers */
#define FS_CAL2_SCALE(rlo, vlo, rhi, vhi)  (((vhi) - (vlo)) / ((rhi) - (rlo)))
#define FS_CAL2_OFFSET(rlo, vlo, rhi, vhi) ((vlo) - (rlo) * FS_CAL2_SCALE(rlo, vlo, rhi, vhi))

typedef struct {
    float        value;
    uint32_t     t_ms;
    fs_quality_t quality;
} fs_sample_t;

int  fs_init(const fs_sensor_t *sensors, int count);
int  fs_start(void);                         /* starts the generic poller       */
int  fs_publish(int ch, float raw);          /* calibrate, validate, store      */
bool fs_get(int ch, fs_sample_t *out);       /* false if no data; flags STALE   */
int  fs_hist_get(int ch, fs_hist_t which, float *out, int max); /* oldest first */
uint32_t fs_hist_period_ms(fs_hist_t which);
const fs_sensor_t *fs_def(int ch);
int  fs_count(void);
int  fs_find(const char *key);

/* each scenario profile implements these */
const fs_sensor_t *fs_profile_sensors(int *count);
void fs_profile_start(void);                 /* extra producers (network, etc.) */

#endif
