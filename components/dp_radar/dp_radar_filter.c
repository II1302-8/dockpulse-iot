// Near-field proximity detector for the HMMD radar.
//
// The module's distance estimator is reliable when a target is close
// (high SNR), so we use it directly: if the reported distance falls
// inside the threshold for N consecutive frames, the berth is
// occupied.

#include "dp_radar_filter.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_SENSOR

#define PROX_CM     CONFIG_DOCKPULSE_PROXIMITY_CM
#define PROX_FRAMES CONFIG_DOCKPULSE_PROXIMITY_STABILITY_FRAMES

static uint8_t s_prox_streak;

bool dp_radar_filter_near(const dp_radar_sample_t *s)
{
    if (!s || !s->presence || s->distance_cm == 0 || s->distance_cm > PROX_CM) {
        s_prox_streak = 0;
        return false;
    }
    if (s_prox_streak < PROX_FRAMES) {
        s_prox_streak++;
    }
    return s_prox_streak >= PROX_FRAMES;
}

#else

bool dp_radar_filter_near(const dp_radar_sample_t *s)
{
    (void)s;
    return false;
}

#endif
