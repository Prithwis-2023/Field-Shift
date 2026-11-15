#ifndef FS_ALARM_H
#define FS_ALARM_H
#include <stdint.h>
#include <stdbool.h>
#include "fs_core.h"

typedef enum { FS_AK_RULE = 0, FS_AK_LOST, FS_AK_FAULT } fs_akind_t;

typedef struct {
    uint16_t    code;
    const char *text;
    int         channel;
    fs_akind_t  kind;
    fs_cmp_t    cmp;
    float       threshold;
    float       hysteresis;
    uint32_t    hold_ms;
    fs_sev_t    sev;
    bool        latching;
} fs_rule_t;

typedef struct {
    const fs_rule_t *rule;
    bool     active;
    bool     latched;
    uint32_t raised_ms;
} fs_alarm_info_t;

void fs_alarm_start(void);                 /* builds the table from the sensor rows */
int  fs_alarm_count(void);
bool fs_alarm_get(int i, fs_alarm_info_t *out);
int  fs_alarm_shown_count(void);           /* active or latched                     */
int  fs_alarm_top(void);                   /* worst one, -1 if none                 */
int  fs_alarm_channel_sev(int ch);         /* worst severity for a channel, -1      */
void fs_alarm_ack(void);
int  fs_alarm_set_threshold(uint16_t code, float thr);  /* runtime edit, 0 = ok     */

#endif
