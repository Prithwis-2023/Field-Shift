#include "fs_forecast.h"

#define HOLT_ALPHA 0.4f
#define HOLT_BETA  0.2f
#define HOLT_PHI   0.97f

int fs_holt(const float *y, int n, int steps, float *out)
{
    if (!y || !out || n < 6 || steps <= 0) return 0;

    float l = y[0];
    float b = (y[5] - y[0]) / 5.0f;
    for (int i = 1; i < n; i++) {
        float lp = l;
        l = HOLT_ALPHA * y[i] + (1.0f - HOLT_ALPHA) * (l + HOLT_PHI * b);
        b = HOLT_BETA * (l - lp) + (1.0f - HOLT_BETA) * HOLT_PHI * b;
    }

    float pw = 1.0f, ps = 0.0f;
    for (int k = 0; k < steps; k++) {
        pw *= HOLT_PHI;
        ps += pw;
        out[k] = l + b * ps;
    }
    return steps;
}

int fs_cross_step(const float *fc, int steps, float thr, bool below)
{
    for (int k = 0; k < steps; k++) {
        if (below ? (fc[k] < thr) : (fc[k] > thr)) return k + 1;
    }
    return 0;
}
