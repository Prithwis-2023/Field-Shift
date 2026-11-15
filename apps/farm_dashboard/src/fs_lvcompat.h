#ifndef FS_LVCOMPAT_H
#define FS_LVCOMPAT_H
#include "lvgl.h"

/* LVGL renamed a few chart calls between v9 releases.
   If the build complains about lv_chart_set_range, set this to 1. */
#ifndef FS_LV_CHART_NEW_API
#define FS_LV_CHART_NEW_API 0
#endif

#if FS_LV_CHART_NEW_API
#define FS_CHART_RANGE(c, lo, hi) lv_chart_set_axis_range((c), LV_CHART_AXIS_PRIMARY_Y, (lo), (hi))
#else
#define FS_CHART_RANGE(c, lo, hi) lv_chart_set_range((c), LV_CHART_AXIS_PRIMARY_Y, (lo), (hi))
#endif

#endif
