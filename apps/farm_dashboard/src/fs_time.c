#include "fs_time.h"
#include "tal_api.h"
#include <stdio.h>

static volatile uint32_t sg_offset;     /* epoch minus uptime (wrapping arithmetic is fine) */
static volatile bool     sg_valid;

int32_t fs_days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void fs_civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp  = (5 * doy + 2) / 153;
    int32_t mm  = mp + (mp < 10 ? 3 : -9);
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mm;
    *y = yoe + era * 400 + (mm <= 2);
}

static uint32_t uptime_s(void) { return (uint32_t)(tal_system_get_millisecond() / 1000); }

void fs_time_set_epoch(uint32_t epoch_s)
{
    sg_offset = epoch_s - uptime_s();
    sg_valid = true;
}

bool     fs_time_valid(void) { return sg_valid; }
uint32_t fs_time_epoch(void) { return sg_offset + uptime_s(); }

bool fs_time_hm(int *hour, int *minute)
{
    if (!sg_valid) return false;
    uint32_t s = fs_time_epoch() % 86400u;
    *hour = (int)(s / 3600u);
    *minute = (int)((s % 3600u) / 60u);
    return true;
}

void fs_time_ymd(char out[9])
{
    int32_t y, m, d;
    if (!sg_valid) {
        snprintf(out, 9, "00000000");
        return;
    }
    fs_civil_from_days((int32_t)(fs_time_epoch() / 86400u), &y, &m, &d);
    snprintf(out, 9, "%04d%02d%02d", (int)y, (int)m, (int)d);
}
