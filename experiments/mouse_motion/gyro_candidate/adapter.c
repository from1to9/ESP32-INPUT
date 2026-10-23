// SPDX-License-Identifier: Apache-2.0
/* Synthetic input adapter. Actual Fusion is a safety reference, not an oracle. */
#include "FusionAhrs.h"
#include "handheld_motion.h"
#include <math.h>
#include <stdio.h>

static void fusion_init(FusionAhrs *ahrs) {
    FusionAhrsInitialise(ahrs);
    const FusionAhrsSettings settings = {
        .sampleRate = 100, .convention = FusionConventionNwu, .gain = 0.5f,
        .gyroscopeRange = 2000, .accelerationRejection = 10,
        .magneticRejection = 0, .rejectionTimeout = 5
    };
    FusionAhrsSetSettings(ahrs, &settings);
    FusionAhrsRestart(ahrs);
}

int main(void) {
    FusionAhrs ahrs;
    handheld_motion_t model;
    const handheld_motion_config_t config = handheld_motion_default_config();
    int current_case = -1;
    double previous_time = 0;
    bool time_seen = false;
    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) return 1;
    puts("case_id,t,vx,vy,ax,ay,rate_x,rate_y,hid_x,hid_y,status,startup,overrange,acceleration_recovery,accel_ignored,accel_error,armed,earth_ax_g,earth_ay_g,earth_az_g,control_vx,control_vy,phase,qw,qx,qy,qz,fusion_earth_ax_g,fusion_earth_ay_g,fusion_earth_az_g");
    while (fgets(line, sizeof(line), stdin)) {
        int case_id, permitted, recenter;
        double t, time, dt, gx, gy, gz, ax, ay, az, tx, ty, vx, vy;
        const int count = sscanf(line, "%d,%lf,%lf,%lf,%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
            &case_id, &t, &time, &dt, &permitted, &recenter, &gx, &gy, &gz,
            &ax, &ay, &az, &tx, &ty, &vx, &vy);
        if (count != 16) return 2;
        if (case_id != current_case) {
            if (!handheld_motion_init(&model, &config)) return 3;
            fusion_init(&ahrs);
            current_case = case_id;
            time_seen = false;
        }
        const bool valid_time = isfinite(time) && isfinite(dt) && dt >= 0.0095 && dt <= 0.0105 &&
            (!time_seen || (time > previous_time && fabs(time - previous_time - dt) < 1e-6));
        previous_time = time;
        time_seen = true;
        const bool valid_data = isfinite(gx) && isfinite(gy) && isfinite(gz) &&
            isfinite(ax) && isfinite(ay) && isfinite(az);
        int status;
        if (!valid_time || !valid_data) {
            FusionAhrsRestart(&ahrs);
            handheld_motion_discontinuity(&model);
            status = valid_time ? 6 : 3;
        } else {
            const FusionVector gyro = {.array = {(float) gx, (float) gy, (float) gz}};
            const FusionVector accel = {.array = {(float) ax, (float) ay, (float) az}};
            FusionAhrsSetSamplePeriod(&ahrs, (float) dt);
            FusionAhrsUpdateNoMagnetometer(&ahrs, gyro, accel);
            const FusionAhrsFlags flags = FusionAhrsGetFlags(&ahrs);
            const bool reference = !flags.startup && !flags.overrangeRecovery && !flags.accelerationRecovery;
            const handheld_motion_sample_t sample = {
                .timestamp_us = (uint64_t) llround(time * 1000000),
                .gyro_dps = {(float) gx, (float) gy, (float) gz},
                .accel_g = {(float) ax, (float) ay, (float) az}
            };
            const bool accepted = handheld_motion_update(&model, &sample,
                permitted && reference, recenter && reference);
            const handheld_motion_output_t *out = handheld_motion_output(&model);
            if (flags.startup) status = 1;
            else if (!reference) status = 4;
            else if (!accepted) status = 6;
            else if (!out->aligned) status = 1;
            else if (!permitted) status = 2;
            else if (recenter) status = 5;
            else if (!out->armed) status = 7;
            else status = 0;
        }
        const handheld_motion_output_t *out = handheld_motion_output(&model);
        const FusionAhrsFlags flags = FusionAhrsGetFlags(&ahrs);
        const FusionAhrsInternalStates internal = FusionAhrsGetInternalStates(&ahrs);
        const FusionVector earth = FusionAhrsGetEarthAcceleration(&ahrs);
        printf("%d,%.9f,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%d,%d,%d,%d,%d,%d,%d,%.9g,%d,%.12g,%.12g,%.12g,%.12g,%.12g,%d,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g\n",
            case_id, t, out->velocity_mps[0], out->velocity_mps[1],
            out->grip_accel_mps2[0], out->grip_accel_mps2[1],
            out->pointer_rate[0], out->pointer_rate[1], out->dx, out->dy, status,
            flags.startup, flags.overrangeRecovery, flags.accelerationRecovery,
            internal.accelerometerIgnored, (double) internal.accelerationError, out->armed,
            out->earth_accel_mps2[0] / 9.80665, out->earth_accel_mps2[1] / 9.80665,
            out->earth_accel_mps2[2] / 9.80665,
            out->pointer_rate[0] / config.pointer_counts_per_meter,
            out->pointer_rate[1] / config.pointer_counts_per_meter, out->phase,
            out->quaternion[0], out->quaternion[1], out->quaternion[2], out->quaternion[3],
            (double) earth.axis.x, (double) earth.axis.y, (double) earth.axis.z);
    }
    return ferror(stdin) || ferror(stdout) ? 4 : 0;
}
