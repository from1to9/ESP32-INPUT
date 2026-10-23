// SPDX-License-Identifier: Apache-2.0
#include "handheld_motion.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static handheld_motion_t model;
static uint64_t timestamp;

static const handheld_motion_output_t *step(double gx, double gy, double gz,
                                             double ax, double ay, double az) {
    timestamp += 10000;
    const handheld_motion_sample_t sample = {
        .timestamp_us = timestamp,
        .gyro_dps = {(float) gx, (float) gy, (float) gz},
        .accel_g = {(float) ax, (float) ay, (float) az}
    };
    assert(handheld_motion_update(&model, &sample, true, false));
    return handheld_motion_output(&model);
}

static void ready(double ax, double ay, double az) {
    const handheld_motion_config_t config = handheld_motion_default_config();
    assert(handheld_motion_init(&model, &config));
    timestamp = 0;
    for (unsigned i = 0; i < 300; ++i) step(0, 0, 0, ax, ay, az);
    assert(model.aligned && !model.armed);
    timestamp += 10000;
    const handheld_motion_sample_t sample = {
        .timestamp_us = timestamp, .accel_g = {(float) ax, (float) ay, (float) az}
    };
    assert(handheld_motion_update(&model, &sample, true, true));
    assert(model.armed && model.output.dx == 0 && model.output.dy == 0);
}

static void onset_coast_and_braking(void) {
    ready(0, 0, 1);
    const handheld_motion_output_t *out = step(0, 0, 0, 0.08 / 9.80665, 0, 1);
    assert(fabs(out->velocity_mps[0] - 0.0008) < 1e-9);
    ready(0, 0, 1);
    for (unsigned i = 0; i < 10; ++i) step(0, 0, 0, 1 / 9.80665, 0, 1);
    assert(fabs(model.output.velocity_mps[0] - 0.1) < 1e-7);
    for (unsigned i = 0; i < 500; ++i) {
        out = step(0, 0, 0, 0, 0, 1);
        assert(fabs(out->velocity_mps[0] - 0.1) < 1e-7);
        assert(out->phase != HANDHELD_MOTION_STOP_PRIOR);
    }
    for (unsigned i = 0; i < 15; ++i) {
        out = step(0, 0, 0, -0.5 / 9.80665, 0, 1);
        assert(out->velocity_mps[0] > 0 && out->pointer_rate[0] > 0);
    }
    for (unsigned i = 0; i < 5; ++i) step(0, 0, 0, -0.5 / 9.80665, 0, 1);
    for (unsigned i = 0; i < 100; ++i) {
        out = step(0, 0, 0, 0, 0, 1);
        assert(out->dx == 0 && out->dy == 0);
        assert(fabs(out->velocity_mps[0]) < 1e-7);
    }
}

static void axis_mapping_and_rotation(void) {
    ready(0, 1, 0);
    for (unsigned i = 0; i < 10; ++i) step(0, 0, 0, 0, 1 + 1 / 9.80665, 0);
    assert(fabs(model.output.velocity_mps[1] - 0.1) < 1e-6);
    assert(fabs(model.output.velocity_mps[0]) < 1e-8);
    ready(0, 0, 1);
    for (unsigned i = 1; i <= 1200; ++i) {
        const double angle = i * 0.01 * 60 * 0.01745329251994329577;
        const handheld_motion_output_t *out = step(60, 0, 0, 0, sin(angle), cos(angle));
        assert(out->dx == 0 && out->dy == 0);
        assert(hypot(out->velocity_mps[0], out->velocity_mps[1]) < 1e-5);
    }
}

static void faults_and_cancellation(void) {
    ready(0, 0, 1);
    for (unsigned i = 0; i < 10; ++i) step(0, 0, 0, 1 / 9.80665, 0, 1);
    handheld_motion_suppress(&model);
    assert(!model.armed && model.aligned && model.output.dx == 0);
    const handheld_motion_output_t *out = step(0, 0, 0, 0, 0, 1);
    assert(!out->armed && out->velocity_mps[0] == 0 && out->dx == 0);
    ready(0, 0, 1);
    timestamp += 20000;
    const handheld_motion_sample_t bad_time = {.timestamp_us = timestamp, .accel_g = {0, 0, 1}};
    assert(!handheld_motion_update(&model, &bad_time, true, false));
    assert(!model.aligned && !model.armed && model.output.dx == 0);
    assert(model.output.phase == HANDHELD_MOTION_SENSOR_FAULT);
    ready(0, 0, 1);
    timestamp += 10000;
    const handheld_motion_sample_t bad_data = {.timestamp_us = timestamp, .accel_g = {NAN, 0, 1}};
    assert(!handheld_motion_update(&model, &bad_data, true, false));
    assert(!model.aligned && !model.armed && model.output.dx == 0);
}

int main(void) {
    onset_coast_and_braking();
    axis_mapping_and_rotation();
    faults_and_cancellation();
    printf("PASS: onset, five-second coast, coherent braking, stop, tilted mapping, sensor-center rotation, cancellation and faults; state=%zu bytes\n", sizeof(model));
    return 0;
}
