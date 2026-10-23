// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Experimental translation observer. Not selected by the production engine.
 * Single coordinator owner; no callbacks, allocation, sensor access or transport.
 * Body axes are a common right-handed frame, gyro in dps, specific force in g.
 * Initial alignment assumes an explicitly requested stationary interval. Neither
 * low acceleration nor this interface independently proves zero velocity. */
typedef struct {
    uint64_t timestamp_us;
    float gyro_dps[3], accel_g[3];
} handheld_motion_sample_t;

typedef enum {
    HANDHELD_MOTION_ALIGNING,
    HANDHELD_MOTION_UNARMED,
    HANDHELD_MOTION_TRACKING,
    HANDHELD_MOTION_BRAKING,
    HANDHELD_MOTION_STOP_PRIOR,
    HANDHELD_MOTION_SENSOR_FAULT
} handheld_motion_phase_t;

typedef struct {
    /* All values are bench hypotheses, not qualified product settings. */
    double minimum_dt, maximum_dt;
    double pointer_counts_per_meter, pointer_deadband_mps, pointer_rate_cap;
    double braking_acceleration_mps2, stop_residual_mps2, stop_speed_mps;
    unsigned alignment_samples, stop_confirmation_samples;
} handheld_motion_config_t;

typedef struct {
    double quaternion[4], gravity_body[3], earth_accel_mps2[3];
    double grip_accel_mps2[2], velocity_mps[2], pointer_rate[2];
    int16_t dx, dy;
    handheld_motion_phase_t phase;
    bool aligned, armed, braking;
} handheld_motion_output_t;

typedef struct {
    handheld_motion_config_t config;
    double quaternion[4], anchor[9], velocity[2], carry[2];
    double alignment_accel[3], alignment_gyro[3], gyro_bias[3];
    double alignment_accel_sq[3], alignment_gyro_sq[3];
    uint64_t previous_us;
    unsigned alignment_count, stop_count;
    bool time_seen, aligned, armed, braking;
    handheld_motion_output_t output;
} handheld_motion_t;

handheld_motion_config_t handheld_motion_default_config(void);
bool handheld_motion_init(handheld_motion_t *state,
                          const handheld_motion_config_t *config);
/* Data loss must restart alignment; never integrate across a missing epoch. */
void handheld_motion_discontinuity(handheld_motion_t *state);
/* Intentional cancellation preserves orientation but invalidates velocity/carry.
 * Recenter is a user-supplied stationary prior, not a sensor rest detector. */
void handheld_motion_suppress(handheld_motion_t *state);
bool handheld_motion_update(handheld_motion_t *state,
                            const handheld_motion_sample_t *sample,
                            bool permitted, bool recenter);
const handheld_motion_output_t *handheld_motion_output(const handheld_motion_t *state);
