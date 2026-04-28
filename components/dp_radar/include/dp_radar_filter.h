#pragma once

#include <stdbool.h>

#include "dp_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when the radar reports a target closer than
// CONFIG_DOCKPULSE_PROXIMITY_CM for at least
// CONFIG_DOCKPULSE_PROXIMITY_STABILITY_FRAMES consecutive frames. Used
// as the berth occupancy signal: something has parked close enough to
// the sensor to count as "in the berth".
bool dp_radar_filter_near(const dp_radar_sample_t *s);

#ifdef __cplusplus
}
#endif
