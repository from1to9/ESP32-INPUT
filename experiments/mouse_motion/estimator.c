/* Local synthetic feasibility prototype. Not production input code. */
#include "FusionAhrs.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { READY, STARTUP, BLOCKED, CADENCE_FAULT, RECOVERY, RECENTERED,
       INVALID_DATA, UNARMED };

typedef struct {
    FusionAhrs ahrs;
    FusionMatrix anchor;
    double velocity[2];
    double carry[2];
    double previous_time;
    bool has_previous;
    bool armed;
} Estimator;

static void clear_motion(Estimator *const state) {
    state->velocity[0] = state->velocity[1] = 0.0;
    state->carry[0] = state->carry[1] = 0.0;
    state->armed = false;
}

static void initialise(Estimator *const state) {
    memset(state, 0, sizeof(*state));
    FusionAhrsInitialise(&state->ahrs);
    const FusionAhrsSettings settings = {
        .sampleRate = 100.0f,
        .convention = FusionConventionNwu,
        .gain = 0.5f,
        .gyroscopeRange = 2000.0f,
        .accelerationRejection = 10.0f,
        .magneticRejection = 0.0f,
        .rejectionTimeout = 5.0f,
    };
    FusionAhrsSetSettings(&state->ahrs, &settings);
    FusionAhrsRestart(&state->ahrs);
}

static bool finite_vector(const FusionVector vector) {
    return isfinite(vector.axis.x) && isfinite(vector.axis.y) &&
           isfinite(vector.axis.z);
}

static int update(Estimator *const state, const bool leaky, const bool oracle,
                  const double time, const double dt, const bool permitted,
                  const bool recenter, const FusionVector gyro,
                  const FusionVector accel, const double truth_accel[2],
                  double acceleration[2], double rate[2], int hid[2]) {
    acceleration[0] = acceleration[1] = 0.0;
    rate[0] = rate[1] = 0.0;
    hid[0] = hid[1] = 0;
    const bool time_valid = isfinite(time) && isfinite(dt) &&
        dt >= 0.0095 && dt <= 0.0105 &&
        (!state->has_previous ||
         (time > state->previous_time &&
          fabs((time - state->previous_time) - dt) < 0.000001));
    state->previous_time = time;
    state->has_previous = true;
    if (!time_valid || !finite_vector(gyro) || !finite_vector(accel)) {
        clear_motion(state);
        FusionAhrsRestart(&state->ahrs);
        return time_valid ? INVALID_DATA : CADENCE_FAULT;
    }
    /* Small clock errors only; startup/rejection still count 100 Hz samples. */
    FusionAhrsSetSamplePeriod(&state->ahrs, (float) dt);
    FusionAhrsUpdateNoMagnetometer(&state->ahrs, gyro, accel);
    const FusionAhrsFlags flags = FusionAhrsGetFlags(&state->ahrs);
    if (flags.startup || flags.overrangeRecovery || flags.accelerationRecovery) {
        clear_motion(state);
        return flags.startup ? STARTUP : RECOVERY;
    }
    if (!permitted) {
        clear_motion(state);
        return BLOCKED;
    }
    if (recenter) {
        clear_motion(state);
        state->anchor = FusionQuaternionToMatrix(FusionAhrsGetQuaternion(&state->ahrs));
        state->armed = true;
        return RECENTERED;
    }
    if (!state->armed) {
        return UNARMED;
    }
    /* Fusion already removes 1 g. No second gravity subtraction. */
    const FusionVector earth = FusionAhrsGetEarthAcceleration(&state->ahrs);
    const FusionMatrix anchor = state->anchor;
    for (int axis = 0; axis < 2; axis++) {
        acceleration[axis] = oracle ? truth_accel[axis] : 9.80665 * (
            anchor.array[axis] * earth.axis.x +
            anchor.array[3 + axis] * earth.axis.y +
            anchor.array[6 + axis] * earth.axis.z);
        /* Every valid sample contributes, including sub-deadband onset impulses. */
        if (leaky) {
            const double decay = exp(-dt / 1.0);
            state->velocity[axis] = decay * state->velocity[axis] +
                1.0 * (1.0 - decay) * acceleration[axis];
        } else {
            state->velocity[axis] += acceleration[axis] * dt;
        }
    }
    if (!isfinite(state->velocity[0]) || !isfinite(state->velocity[1])) {
        clear_motion(state);
        return INVALID_DATA;
    }
    const double speed = hypot(state->velocity[0], state->velocity[1]);
    const double scale = speed < 0.01 ? 0.0 : fmin(1000.0, 2000.0 / speed);
    for (int axis = 0; axis < 2; axis++) {
        rate[axis] = scale * state->velocity[axis];
        state->carry[axis] += rate[axis] * dt;
        hid[axis] = (int) trunc(state->carry[axis]);
        state->carry[axis] -= hid[axis];
    }
    return READY;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "integrate") && strcmp(argv[1], "leaky")) ||
        (strcmp(argv[2], "fusion") && strcmp(argv[2], "oracle"))) {
        return 2;
    }
    const bool leaky = strcmp(argv[1], "leaky") == 0;
    const bool oracle = strcmp(argv[2], "oracle") == 0;
    Estimator state;
    int current_case = -1;
    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) {
        return 3;
    }
    puts("case_id,t,vx,vy,ax,ay,rate_x,rate_y,hid_x,hid_y,status,startup,overrange,acceleration_recovery,accel_ignored,accel_error,armed,earth_ax_g,earth_ay_g,earth_az_g,control_vx,control_vy");
    while (fgets(line, sizeof(line), stdin)) {
        int case_id, permitted, recenter;
        double t, time, dt, gx, gy, gz, ax, ay, az, tx, ty, vx, vy;
        const int read = sscanf(line, "%d,%lf,%lf,%lf,%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
            &case_id, &t, &time, &dt, &permitted, &recenter,
            &gx, &gy, &gz, &ax, &ay, &az, &tx, &ty, &vx, &vy);
        if (read != 16) {
            fprintf(stderr, "Invalid fixture row: %d fields\n", read);
            return 4;
        }
        if (case_id != current_case) {
            initialise(&state);
            current_case = case_id;
        }
        const FusionVector gyro = {.array = {(float) gx, (float) gy, (float) gz}};
        const FusionVector accel = {.array = {(float) ax, (float) ay, (float) az}};
        const double truth_accel[2] = {tx, ty};
        double acceleration[2], rate[2];
        int hid[2];
        const int status = update(&state, leaky, oracle, time, dt,
            permitted != 0, recenter != 0, gyro, accel, truth_accel,
            acceleration, rate, hid);
        const FusionAhrsFlags flags = FusionAhrsGetFlags(&state.ahrs);
        const FusionAhrsInternalStates internal = FusionAhrsGetInternalStates(&state.ahrs);
        const FusionVector earth = FusionAhrsGetEarthAcceleration(&state.ahrs);
        printf("%d,%.9f,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%d,%d,%d,%d,%d,%d,%d,%.9g,%d,%.12g,%.12g,%.12g,%.12g,%.12g\n",
            case_id, t, state.velocity[0], state.velocity[1],
            acceleration[0], acceleration[1], rate[0], rate[1], hid[0], hid[1],
            status, flags.startup, flags.overrangeRecovery, flags.accelerationRecovery,
            internal.accelerometerIgnored, (double) internal.accelerationError, state.armed,
            (double) earth.axis.x, (double) earth.axis.y, (double) earth.axis.z,
            rate[0] / 1000.0, rate[1] / 1000.0);
    }
    if (ferror(stdin) || ferror(stdout)) {
        return 5;
    }
    return 0;
}
