#ifndef FS_FORECAST_H
#define FS_FORECAST_H
#include <stdbool.h>

/* damped Holt smoothing: returns number of forecast points written (0 if too little data) */
int fs_holt(const float *y, int n, int steps, float *out);

/* first forecast step (1-based) that crosses thr, or 0 if none */
int fs_cross_step(const float *fc, int steps, float thr, bool below);

#endif
