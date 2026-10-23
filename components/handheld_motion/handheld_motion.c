// SPDX-License-Identifier: Apache-2.0
#include "handheld_motion.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define STANDARD_GRAVITY 9.80665
#define RADIANS_PER_DEGREE 0.01745329251994329577

static double norm(const double *v, unsigned count) {
    double sum = 0;
    for (unsigned i = 0; i < count; ++i) sum += v[i] * v[i];
    return sqrt(sum);
}

static void matrix(const double q[4], double m[9]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double values[9] = {
        1 - 2 * (y*y + z*z), 2 * (x*y - w*z), 2 * (x*z + w*y),
        2 * (x*y + w*z), 1 - 2 * (x*x + z*z), 2 * (y*z - w*x),
        2 * (x*z - w*y), 2 * (y*z + w*x), 1 - 2 * (x*x + y*y)
    };
    memcpy(m, values, sizeof(values));
}

handheld_motion_config_t handheld_motion_default_config(void) {
    return (handheld_motion_config_t) {
        .minimum_dt = 0.0095, .maximum_dt = 0.0105,
        .pointer_counts_per_meter = 2000, .pointer_deadband_mps = 0.01,
        .pointer_rate_cap = 2000, .braking_acceleration_mps2 = 0.05,
        .stop_residual_mps2 = 0.12, .stop_speed_mps = 0.01,
        .alignment_samples = 300, .stop_confirmation_samples = 4
    };
}

void handheld_motion_suppress(handheld_motion_t *s) {
    s->velocity[0] = s->velocity[1] = 0;
    s->carry[0] = s->carry[1] = 0;
    s->stop_count = 0;
    s->armed = s->braking = false;
    s->output.velocity_mps[0] = s->output.velocity_mps[1] = 0;
    s->output.pointer_rate[0] = s->output.pointer_rate[1] = 0;
    s->output.dx = s->output.dy = 0;
    s->output.armed = s->output.braking = false;
    s->output.phase = s->aligned ? HANDHELD_MOTION_UNARMED : HANDHELD_MOTION_ALIGNING;
}

void handheld_motion_discontinuity(handheld_motion_t *s) {
    const handheld_motion_config_t config = s->config;
    memset(s, 0, sizeof(*s));
    s->config = config;
    s->quaternion[0] = 1;
}

bool handheld_motion_init(handheld_motion_t *s, const handheld_motion_config_t *c) {
    if (!s || !c || !isfinite(c->minimum_dt) || !isfinite(c->maximum_dt) ||
        c->minimum_dt <= 0 || c->maximum_dt < c->minimum_dt || c->maximum_dt > 0.05 ||
        !isfinite(c->pointer_counts_per_meter) || c->pointer_counts_per_meter <= 0 ||
        !isfinite(c->pointer_deadband_mps) || c->pointer_deadband_mps < 0 ||
        !isfinite(c->pointer_rate_cap) || c->pointer_rate_cap <= 0 ||
        c->pointer_rate_cap * c->maximum_dt > 126 ||
        !isfinite(c->braking_acceleration_mps2) || c->braking_acceleration_mps2 <= 0 ||
        !isfinite(c->stop_residual_mps2) || c->stop_residual_mps2 <= 0 ||
        !isfinite(c->stop_speed_mps) || c->stop_speed_mps <= 0 ||
        c->alignment_samples < 2 || c->alignment_samples > 10000 ||
        !c->stop_confirmation_samples || c->stop_confirmation_samples > 1000) return false;
    memset(s, 0, sizeof(*s));
    s->config = *c;
    s->quaternion[0] = 1;
    return true;
}

static bool align(handheld_motion_t *s, const double a[3], const double g[3]) {
    if (norm(g, 3) > 3 || fabs(norm(a, 3) - 1) > 0.03) {
        s->alignment_count = 0;
        memset(s->alignment_accel, 0, sizeof(s->alignment_accel));
        memset(s->alignment_gyro, 0, sizeof(s->alignment_gyro));
        memset(s->alignment_accel_sq, 0, sizeof(s->alignment_accel_sq));
        memset(s->alignment_gyro_sq, 0, sizeof(s->alignment_gyro_sq));
        return false;
    }
    ++s->alignment_count;
    for (unsigned i = 0; i < 3; ++i) {
        s->alignment_accel[i] += a[i];
        s->alignment_gyro[i] += g[i];
        s->alignment_accel_sq[i] += a[i] * a[i];
        s->alignment_gyro_sq[i] += g[i] * g[i];
    }
    if (s->alignment_count < s->config.alignment_samples) return false;
    double mean[3];
    for (unsigned i = 0; i < 3; ++i) {
        mean[i] = s->alignment_accel[i] / s->alignment_count;
        s->gyro_bias[i] = s->alignment_gyro[i] / s->alignment_count;
        const double avar = s->alignment_accel_sq[i] / s->alignment_count - mean[i] * mean[i];
        const double gvar = s->alignment_gyro_sq[i] / s->alignment_count - s->gyro_bias[i] * s->gyro_bias[i];
        if (avar > 0.0001 || gvar > 0.04) {
            handheld_motion_discontinuity(s);
            return false;
        }
    }
    const double magnitude = norm(mean, 3);
    for (unsigned i = 0; i < 3; ++i) mean[i] /= magnitude;
    /* Shortest body-gravity -> world-up rotation. Six-axis yaw stays arbitrary;
     * the same heading is retained in the grip anchor and cancels in mapping. */
    if (mean[2] < -0.999999) {
        s->quaternion[0] = 0; s->quaternion[1] = 1;
        s->quaternion[2] = s->quaternion[3] = 0;
    } else {
        s->quaternion[0] = sqrt((1 + mean[2]) / 2);
        s->quaternion[1] = mean[1] / (2 * s->quaternion[0]);
        s->quaternion[2] = -mean[0] / (2 * s->quaternion[0]);
        s->quaternion[3] = 0;
    }
    s->aligned = true;
    return true;
}

static void propagate(handheld_motion_t *s, const double gyro[3], double dt) {
    double angle[3];
    for (unsigned i = 0; i < 3; ++i) angle[i] = (gyro[i] - s->gyro_bias[i]) * RADIANS_PER_DEGREE * dt;
    const double theta = norm(angle, 3);
    const double factor = theta > 1e-12 ? sin(theta / 2) / theta : 0.5;
    const double r[4] = {cos(theta / 2), angle[0] * factor, angle[1] * factor, angle[2] * factor};
    const double *q = s->quaternion;
    const double next[4] = {
        q[0]*r[0] - q[1]*r[1] - q[2]*r[2] - q[3]*r[3],
        q[0]*r[1] + q[1]*r[0] + q[2]*r[3] - q[3]*r[2],
        q[0]*r[2] - q[1]*r[3] + q[2]*r[0] + q[3]*r[1],
        q[0]*r[3] + q[1]*r[2] - q[2]*r[1] + q[3]*r[0]
    };
    const double magnitude = norm(next, 4);
    for (unsigned i = 0; i < 4; ++i) s->quaternion[i] = next[i] / magnitude;
}

bool handheld_motion_update(handheld_motion_t *s, const handheld_motion_sample_t *sample,
                            bool permitted, bool recenter) {
    if (!s || !sample) return false;
    memset(&s->output, 0, sizeof(s->output));
    double dt = 0.01, a[3], g[3];
    bool valid = true;
    if (s->time_seen) {
        valid = sample->timestamp_us > s->previous_us;
        if (valid) dt = (sample->timestamp_us - s->previous_us) / 1000000.0;
        valid = valid && dt >= s->config.minimum_dt && dt <= s->config.maximum_dt;
    }
    for (unsigned i = 0; i < 3; ++i) {
        a[i] = sample->accel_g[i]; g[i] = sample->gyro_dps[i];
        valid = valid && isfinite(a[i]) && isfinite(g[i]) && fabs(g[i]) <= 1960;
    }
    const double anorm = norm(a, 3);
    valid = valid && anorm >= 0.25 && anorm <= 2.5;
    if (!valid) {
        handheld_motion_discontinuity(s);
        s->output.phase = HANDHELD_MOTION_SENSOR_FAULT;
        return false;
    }
    s->previous_us = sample->timestamp_us;
    s->time_seen = true;
    if (!s->aligned) {
        align(s, a, g);
    } else {
        propagate(s, g, dt);
    }
    memcpy(s->output.quaternion, s->quaternion, sizeof(s->quaternion));
    s->output.aligned = s->aligned;
    if (!s->aligned) return true;
    double rotation[9];
    matrix(s->quaternion, rotation);
    for (unsigned i = 0; i < 3; ++i) {
        s->output.gravity_body[i] = rotation[6 + i];
        s->output.earth_accel_mps2[i] = STANDARD_GRAVITY * (
            rotation[3*i] * a[0] + rotation[3*i+1] * a[1] + rotation[3*i+2] * a[2] - (i == 2 ? 1.0 : 0.0));
    }
    if (!permitted) { handheld_motion_suppress(s); return true; }
    if (recenter) {
        handheld_motion_suppress(s);
        memcpy(s->anchor, rotation, sizeof(rotation));
        s->armed = s->output.armed = true;
        return true;
    }
    if (!s->armed) { s->output.phase = HANDHELD_MOTION_UNARMED; return true; }
    const double before_speed = norm(s->velocity, 2);
    double acceleration[2];
    for (unsigned axis = 0; axis < 2; ++axis) {
        acceleration[axis] = 0;
        for (unsigned i = 0; i < 3; ++i) acceleration[axis] += s->anchor[3*i+axis] * s->output.earth_accel_mps2[i];
        s->output.grip_accel_mps2[axis] = acceleration[axis];
    }
    const double energy_rate = s->velocity[0] * acceleration[0] + s->velocity[1] * acceleration[1];
    const double accel_norm = norm(acceleration, 2);
    if (before_speed >= 0.02 && energy_rate < -s->config.braking_acceleration_mps2 * before_speed) s->braking = true;
    if (accel_norm > 0.2 && energy_rate > 0) s->braking = false;
    for (unsigned i = 0; i < 2; ++i) s->velocity[i] += acceleration[i] * dt;
    double speed = norm(s->velocity, 2);
    if (s->braking && accel_norm <= s->config.stop_residual_mps2 && speed <= s->config.stop_speed_mps) {
        ++s->stop_count;
    } else {
        s->stop_count = 0;
    }
    bool stopped = false;
    if (s->stop_count >= s->config.stop_confirmation_samples) {
        s->velocity[0] = s->velocity[1] = s->carry[0] = s->carry[1] = 0;
        speed = 0;
        stopped = true;
        s->stop_count = 0;
        s->braking = false;
    }
    const double scale = speed < s->config.pointer_deadband_mps ? 0 : fmin(s->config.pointer_counts_per_meter, s->config.pointer_rate_cap / speed);
    int16_t hid[2];
    for (unsigned i = 0; i < 2; ++i) {
        s->output.velocity_mps[i] = s->velocity[i];
        s->output.pointer_rate[i] = scale * s->velocity[i];
        s->carry[i] += s->output.pointer_rate[i] * dt;
        hid[i] = (int16_t) trunc(s->carry[i]);
        s->carry[i] -= hid[i];
    }
    s->output.dx = hid[0]; s->output.dy = hid[1];
    s->output.armed = true; s->output.braking = s->braking;
    s->output.phase = stopped ? HANDHELD_MOTION_STOP_PRIOR : (s->braking ? HANDHELD_MOTION_BRAKING : HANDHELD_MOTION_TRACKING);
    return true;
}

const handheld_motion_output_t *handheld_motion_output(const handheld_motion_t *s) {
    return s ? &s->output : NULL;
}
