#ifndef FS_TIME_H
#define FS_TIME_H
#include <stdint.h>
#include <stdbool.h>

int32_t fs_days_from_civil(int32_t y, int32_t m, int32_t d);
void    fs_civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d);

void     fs_time_set_epoch(uint32_t epoch_s);   /* UTC seconds since 1970 */
bool     fs_time_valid(void);
uint32_t fs_time_epoch(void);
bool     fs_time_hm(int *hour, int *minute);    /* UTC; false if not synced yet */
void     fs_time_ymd(char out[9]);              /* "YYYYMMDD" UTC, "00000000" if not synced */

#endif
