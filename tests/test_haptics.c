// SPDX-License-Identifier: Apache-2.0
#include "haptics.h"
#include "haptics_dispatch.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t time_ms;
    uint8_t strength;
    bool accepted;
} write_record_t;

typedef struct {
    uint64_t time_ms;
    uint8_t output;
    unsigned fail_on;
    unsigned fail_off;
    bool change_before_failure;
    size_t count;
    write_record_t records[2048];
} motor_t;

static haptics_write_result_t write_motor(void *context, uint8_t strength)
{
    motor_t *motor = context;
    assert(strength <= 100);
    assert(motor->count < sizeof(motor->records) / sizeof(motor->records[0]));
    unsigned *failures = strength ? &motor->fail_on : &motor->fail_off;
    bool accepted = *failures == 0;
    if (!accepted) {
        --*failures;
    }
    if (accepted || motor->change_before_failure) {
        motor->output = strength;
    }
    motor->records[motor->count++] = (write_record_t){motor->time_ms, strength, accepted};
    return accepted ? HAPTICS_WRITE_OK : HAPTICS_WRITE_FAILED;
}

static void initialize(haptics_t *state, motor_t *motor, uint64_t time_ms)
{
    memset(motor, 0, sizeof(*motor));
    motor->time_ms = time_ms;
    motor->output = 90; /* Initialization must establish off, not assume it. */
    assert(haptics_init(state, true, write_motor, motor, time_ms) == HAPTICS_OK);
    assert(haptics_available(state));
    assert(haptics_enabled(state));
    assert(!haptics_active(state));
    assert(motor->output == 0);
    assert(motor->count == 1);
}

static haptics_result_t request(haptics_t *state, motor_t *motor,
    haptics_event_t event, uint64_t time_ms)
{
    motor->time_ms = time_ms;
    return haptics_request(state, event, time_ms);
}

static haptics_result_t poll(haptics_t *state, motor_t *motor, uint64_t time_ms)
{
    motor->time_ms = time_ms;
    return haptics_poll(state, time_ms);
}

static haptics_result_t enable(haptics_t *state, motor_t *motor,
    bool enabled, uint64_t time_ms)
{
    motor->time_ms = time_ms;
    return haptics_set_enabled(state, enabled, time_ms);
}

typedef enum { CONTROL_NONE, CONTROL_DISABLE, CONTROL_DISABLE_ENABLE } control_injection_t;

typedef struct {
    haptics_dispatch_t dispatch;
    haptics_t policy;
    motor_t motor;
    uint64_t worker_generation;
    bool held;
    control_injection_t before_lock;
    control_injection_t during_write;
    control_injection_t deferred;
    unsigned sequence;
    unsigned last_on_sequence;
    unsigned last_control_sequence;
} dispatch_fixture_t;

static void injected_control_locked(dispatch_fixture_t *fixture, control_injection_t injection)
{
    assert(fixture->held);
    assert(injection != CONTROL_NONE);
    assert(haptics_dispatch_enable_locked(&fixture->dispatch, false));
    fixture->last_control_sequence = ++fixture->sequence;
    if (injection == CONTROL_DISABLE_ENABLE) {
        assert(haptics_dispatch_enable_locked(&fixture->dispatch, true));
        fixture->last_control_sequence = ++fixture->sequence;
    }
}

static void dispatch_enter(void *context)
{
    dispatch_fixture_t *fixture = context;
    assert(!fixture->held);
    /* Deterministic competing task: win the shared lock immediately before
     * motor dispatch, after the worker has already captured its generation. */
    if (fixture->before_lock != CONTROL_NONE) {
        const control_injection_t injection = fixture->before_lock;
        fixture->before_lock = CONTROL_NONE;
        fixture->held = true;
        injected_control_locked(fixture, injection);
        fixture->held = false;
    }
    fixture->held = true;
}

static void dispatch_leave(void *context)
{
    dispatch_fixture_t *fixture = context;
    assert(fixture->held);
    fixture->held = false;
    /* A control request arriving while the driver holds the lock can commit
     * only after the hardware write. This models the other serialization. */
    if (fixture->deferred != CONTROL_NONE) {
        const control_injection_t injection = fixture->deferred;
        fixture->deferred = CONTROL_NONE;
        fixture->held = true;
        injected_control_locked(fixture, injection);
        fixture->held = false;
    }
}

static haptics_write_result_t dispatch_driver_write(void *context, uint8_t strength)
{
    dispatch_fixture_t *fixture = context;
    /* This assertion catches separating the epoch check from driver access. */
    assert(fixture->held);
    if (strength) {
        assert(fixture->dispatch.enabled && !fixture->dispatch.exhausted);
        assert(fixture->worker_generation == fixture->dispatch.generation);
        if (fixture->during_write != CONTROL_NONE) {
            fixture->deferred = fixture->during_write;
            fixture->during_write = CONTROL_NONE;
        }
        fixture->last_on_sequence = ++fixture->sequence;
    }
    return write_motor(&fixture->motor, strength);
}

static haptics_write_result_t dispatch_policy_write(void *context, uint8_t strength)
{
    dispatch_fixture_t *fixture = context;
    return haptics_dispatch_write(&fixture->dispatch, fixture->worker_generation, strength);
}

static void dispatch_initialize(dispatch_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->motor.output = 90;
    assert(haptics_dispatch_init(&fixture->dispatch, dispatch_enter, dispatch_leave,
        fixture, dispatch_driver_write, fixture));
    assert(haptics_init(&fixture->policy, true, dispatch_policy_write, fixture, 0) == HAPTICS_OK);
    dispatch_enter(fixture);
    assert(haptics_dispatch_enable_locked(&fixture->dispatch, true));
    fixture->worker_generation = fixture->dispatch.generation;
    dispatch_leave(fixture);
    assert(fixture->motor.output == 0);
}

static void test_atomic_dispatch_cancellation(void)
{
    dispatch_fixture_t fixture;
    for (control_injection_t injection = CONTROL_DISABLE;
            injection <= CONTROL_DISABLE_ENABLE; ++injection) {
        dispatch_initialize(&fixture);
        fixture.before_lock = injection;
        const size_t writes = fixture.motor.count;
        assert(haptics_request(&fixture.policy, HAPTICS_CONFIRM, 0) == HAPTICS_CANCELLED);
        assert(fixture.motor.count == writes && fixture.motor.output == 0);
        assert(haptics_available(&fixture.policy));
        assert(!haptics_enabled(&fixture.policy) && !haptics_active(&fixture.policy));
        assert(!fixture.held);
    }

    dispatch_initialize(&fixture);
    assert(haptics_request(&fixture.policy, HAPTICS_ERROR, 0) == HAPTICS_OK);
    fixture.motor.time_ms = 40;
    assert(haptics_poll(&fixture.policy, 40) == HAPTICS_OK && fixture.motor.output == 0);
    fixture.before_lock = CONTROL_DISABLE_ENABLE;
    const size_t writes = fixture.motor.count;
    fixture.motor.time_ms = 105;
    assert(haptics_poll(&fixture.policy, 105) == HAPTICS_CANCELLED);
    assert(fixture.motor.count == writes && fixture.motor.output == 0);
    assert(haptics_available(&fixture.policy) && !haptics_enabled(&fixture.policy));
    assert(haptics_set_enabled(&fixture.policy, true, 106) == HAPTICS_OK);
    assert(haptics_poll(&fixture.policy, 365) == HAPTICS_OK && fixture.motor.count == writes);
    /* Even while enabled again, the original worker generation is stale. */
    assert(haptics_request(&fixture.policy, HAPTICS_MODE, 365) == HAPTICS_CANCELLED);
    assert(fixture.motor.count == writes && haptics_available(&fixture.policy));
    fixture.worker_generation = fixture.dispatch.generation;
    assert(haptics_set_enabled(&fixture.policy, true, 800) == HAPTICS_OK);
    fixture.motor.time_ms = 800;
    assert(haptics_request(&fixture.policy, HAPTICS_MODE, 800) == HAPTICS_OK);
    assert(fixture.motor.output > 0);

    dispatch_initialize(&fixture);
    fixture.during_write = CONTROL_DISABLE_ENABLE;
    assert(haptics_request(&fixture.policy, HAPTICS_LOCK, 0) == HAPTICS_OK);
    assert(fixture.motor.output > 0);
    assert(fixture.last_on_sequence < fixture.last_control_sequence);
    /* This write committed before cancellation. The next worker control step
     * stops it; every subsequent nonzero phase still uses the old token. */
    fixture.motor.time_ms = 1;
    assert(haptics_set_enabled(&fixture.policy, false, 1) == HAPTICS_OK);
    assert(fixture.motor.output == 0);
    assert(haptics_poll(&fixture.policy, 1000) == HAPTICS_OK && fixture.motor.output == 0);
}

static void test_dispatch_failures_and_generation_exhaustion(void)
{
    dispatch_fixture_t fixture;
    dispatch_initialize(&fixture);
    fixture.motor.fail_on = 1;
    fixture.motor.change_before_failure = true;
    assert(haptics_request(&fixture.policy, HAPTICS_LOCK, 0) == HAPTICS_HARDWARE_ERROR);
    assert(!haptics_available(&fixture.policy) && fixture.motor.output == 0);

    dispatch_initialize(&fixture);
    const size_t writes = fixture.motor.count;
    assert(haptics_dispatch_write(&fixture.dispatch, fixture.worker_generation, 101) == HAPTICS_WRITE_FAILED);
    assert(fixture.motor.count == writes && !fixture.held);
    dispatch_enter(&fixture);
    fixture.dispatch.generation = UINT64_MAX;
    assert(haptics_dispatch_enable_locked(&fixture.dispatch, false));
    assert(fixture.dispatch.exhausted && !fixture.dispatch.enabled);
    assert(!haptics_dispatch_enable_locked(&fixture.dispatch, true));
    dispatch_leave(&fixture);
    assert(haptics_dispatch_write(&fixture.dispatch, UINT64_MAX, 35) == HAPTICS_WRITE_CANCELLED);
    assert(fixture.motor.count == writes);
    /* Cancellation and exhausted tokens never prevent a real off write. */
    assert(haptics_dispatch_write(&fixture.dispatch, 0, 0) == HAPTICS_WRITE_OK);
    assert(fixture.motor.count == writes + 1 && fixture.motor.output == 0);
    assert(!haptics_dispatch_init(NULL, dispatch_enter, dispatch_leave,
        &fixture, dispatch_driver_write, &fixture));
    assert(!haptics_dispatch_init(&fixture.dispatch, NULL, dispatch_leave,
        &fixture, dispatch_driver_write, &fixture));
    assert(haptics_dispatch_write(&fixture.dispatch, 0, 0) == HAPTICS_WRITE_FAILED);
}

static void test_required_motor_evidence(void)
{
    haptics_t state;
    motor_t motor = {0};
    assert(haptics_init(&state, false, write_motor, &motor, 0) == HAPTICS_UNAVAILABLE);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1) == HAPTICS_UNAVAILABLE);
    assert(enable(&state, &motor, true, 2) == HAPTICS_UNAVAILABLE);
    assert(haptics_recover(&state, 3) == HAPTICS_UNAVAILABLE);
    assert(!haptics_available(&state) && !haptics_enabled(&state));
    assert(motor.count == 0);
    assert(haptics_init(&state, true, NULL, NULL, 0) == HAPTICS_UNAVAILABLE);
    assert(haptics_init(NULL, true, write_motor, &motor, 0) == HAPTICS_UNAVAILABLE);
    assert(haptics_poll(NULL, 0) == HAPTICS_UNAVAILABLE);
    assert(haptics_request(NULL, HAPTICS_ERROR, 0) == HAPTICS_UNAVAILABLE);
    assert(haptics_set_enabled(NULL, false, 0) == HAPTICS_UNAVAILABLE);
    assert(!haptics_active(NULL));

    motor.output = 90;
    motor.fail_off = 3;
    assert(haptics_init(&state, true, write_motor, &motor, 0) == HAPTICS_HARDWARE_ERROR);
    assert(!haptics_available(&state) && haptics_active(&state));
    assert(motor.count == 2); /* Initial off, then immediate safety attempt. */
    assert(poll(&state, &motor, 19) == HAPTICS_HARDWARE_ERROR);
    assert(motor.count == 2);
    assert(poll(&state, &motor, 20) == HAPTICS_HARDWARE_ERROR);
    assert(motor.count == 3 && motor.output == 90);
    assert(poll(&state, &motor, 40) == HAPTICS_HARDWARE_ERROR);
    assert(motor.output == 0 && !haptics_active(&state));
    assert(!haptics_available(&state)); /* A successful stop does not hide a fault. */
}

static void test_pulse_boundaries_and_strength(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 1000);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1000) == HAPTICS_OK);
    const uint8_t mild = motor.output;
    assert(mild > 0 && mild < 100);
    assert(poll(&state, &motor, 1023) == HAPTICS_OK);
    assert(motor.output == mild);
    assert(poll(&state, &motor, 1024) == HAPTICS_OK);
    assert(motor.output == 0);
    assert(request(&state, &motor, HAPTICS_MODE, 1223) == HAPTICS_SUPPRESSED);
    assert(request(&state, &motor, HAPTICS_MODE, 1224) == HAPTICS_OK);
    const uint8_t mode_strength = motor.output;
    assert(mode_strength > mild && mode_strength < 100);
    assert(poll(&state, &motor, 1268) == HAPTICS_OK && motor.output == mode_strength);
    assert(poll(&state, &motor, 1269) == HAPTICS_OK && motor.output == 0);
    assert(request(&state, &motor, HAPTICS_LOCK, 1469) == HAPTICS_OK);
    assert(motor.output >= mode_strength && motor.output < 100);
    assert(poll(&state, &motor, 1533) == HAPTICS_OK && motor.output != 0);
    assert(poll(&state, &motor, 1534) == HAPTICS_OK && motor.output == 0);
    assert(request(&state, &motor, (haptics_event_t)-1, 1734) == HAPTICS_INVALID_EVENT);
    assert(request(&state, &motor, (haptics_event_t)999, 1734) == HAPTICS_INVALID_EVENT);
    assert(motor.output == 0);
}

static void test_distinct_error_and_late_poll(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_ERROR, 0) == HAPTICS_OK);
    assert(poll(&state, &motor, 39) == HAPTICS_OK && motor.output > 0);
    assert(poll(&state, &motor, 40) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 104) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 105) == HAPTICS_OK && motor.output > 0);
    assert(poll(&state, &motor, 164) == HAPTICS_OK && motor.output > 0);
    assert(poll(&state, &motor, 165) == HAPTICS_OK && motor.output == 0);
    assert(motor.count == 5); /* Known off, first on/off, second on/off. */

    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_ERROR, 0) == HAPTICS_OK);
    assert(poll(&state, &motor, 5000) == HAPTICS_OK && motor.output == 0);
    assert(motor.count == 3); /* No replay of the missed second pulse. */
    assert(poll(&state, &motor, 6000) == HAPTICS_OK && motor.output == 0);
    assert(motor.count == 3);

    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_ERROR, 0) == HAPTICS_OK);
    assert(poll(&state, &motor, 120) == HAPTICS_OK && motor.output == 0);
    const size_t writes = motor.count;
    assert(poll(&state, &motor, 121) == HAPTICS_OK && motor.count == writes);
    assert(poll(&state, &motor, 164) == HAPTICS_OK && motor.output == 0);

    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_ERROR, 0) == HAPTICS_OK);
    assert(poll(&state, &motor, 80) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 105) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 144) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 145) == HAPTICS_OK && motor.output > 0);
    assert(poll(&state, &motor, 165) == HAPTICS_OK && motor.output == 0);

    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 0) == HAPTICS_OK);
    assert(poll(&state, &motor, 5000) == HAPTICS_OK && motor.output == 0);
    assert(request(&state, &motor, HAPTICS_LOCK, 5000) == HAPTICS_SUPPRESSED);
    assert(request(&state, &motor, HAPTICS_LOCK, 5199) == HAPTICS_SUPPRESSED);
    assert(request(&state, &motor, HAPTICS_LOCK, 5200) == HAPTICS_OK);
}

static void test_flood_does_not_extend_or_queue(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 0);
    unsigned accepted = 0;
    for (uint64_t time_ms = 0; time_ms < 2000; ++time_ms) {
        haptics_result_t result = request(&state, &motor, HAPTICS_ERROR, time_ms);
        assert(result == HAPTICS_OK || result == HAPTICS_SUPPRESSED);
        accepted += result == HAPTICS_OK;
    }
    assert(accepted > 1 && accepted < 8);
    assert(poll(&state, &motor, 2500) == HAPTICS_OK);
    assert(motor.output == 0);

    uint64_t on_since = 0;
    uint64_t off_since = 0;
    uint64_t total_on_ms = 0;
    bool on = false;
    bool had_pulse = false;
    for (size_t i = 1; i < motor.count; ++i) {
        const write_record_t record = motor.records[i];
        assert(record.accepted);
        if (record.strength) {
            assert(!on);
            if (had_pulse) {
                assert(record.time_ms - off_since >= 65);
            }
            on_since = record.time_ms;
            on = true;
            had_pulse = true;
        } else {
            assert(on);
            assert(record.time_ms - on_since <= 65);
            total_on_ms += record.time_ms - on_since;
            off_since = record.time_ms;
            on = false;
        }
    }
    assert(!on && total_on_ms <= 600);
    const size_t writes = motor.count;
    assert(poll(&state, &motor, 10000) == HAPTICS_OK);
    assert(motor.count == writes); /* Suppressed requests do not become future work. */
}

static void test_disable_cancels_without_replay(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 0);
    assert(request(&state, &motor, HAPTICS_ERROR, 0) == HAPTICS_OK);
    assert(enable(&state, &motor, false, 10) == HAPTICS_OK);
    assert(motor.output == 0 && !haptics_active(&state));
    assert(!haptics_enabled(&state));
    assert(request(&state, &motor, HAPTICS_LOCK, 20) == HAPTICS_DISABLED);
    assert(enable(&state, &motor, true, 30) == HAPTICS_OK);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 30) == HAPTICS_SUPPRESSED);
    const size_t writes = motor.count;
    assert(poll(&state, &motor, 105) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 1000) == HAPTICS_OK && motor.count == writes);
    assert(request(&state, &motor, HAPTICS_ERROR, 1000) == HAPTICS_OK);
    assert(poll(&state, &motor, 1040) == HAPTICS_OK && motor.output == 0);
    assert(enable(&state, &motor, false, 1105) == HAPTICS_OK);
    assert(enable(&state, &motor, true, 1105) == HAPTICS_OK);
    assert(poll(&state, &motor, 1106) == HAPTICS_OK && motor.output == 0);
    assert(poll(&state, &motor, 1200) == HAPTICS_OK && motor.output == 0);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1364) == HAPTICS_SUPPRESSED);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1365) == HAPTICS_OK);

    /* Cancelling an overdue physical pulse must retain a real off interval. */
    assert(enable(&state, &motor, false, 10000) == HAPTICS_OK);
    assert(enable(&state, &motor, true, 10000) == HAPTICS_OK);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 10199) == HAPTICS_SUPPRESSED);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 10200) == HAPTICS_OK);
}

static void test_continuous_operation_across_32_bit_time(void)
{
    haptics_t state;
    motor_t motor;
    const uint64_t base = (uint64_t)UINT32_MAX - 1000;
    initialize(&state, &motor, base);
    unsigned accepted = 0;
    for (uint64_t offset = 0; offset < 100000; ++offset) {
        const haptics_result_t result = request(&state, &motor, HAPTICS_ERROR, base + offset);
        assert(result == HAPTICS_OK || result == HAPTICS_SUPPRESSED);
        accepted += result == HAPTICS_OK;
    }
    for (uint64_t offset = 100000; offset < 100200; ++offset) {
        assert(poll(&state, &motor, base + offset) == HAPTICS_OK);
    }
    assert(accepted > 200 && accepted < 300);
    assert(haptics_available(&state) && haptics_enabled(&state));
    assert(motor.output == 0 && !haptics_active(&state));
    uint64_t on_since = 0;
    uint64_t off_since = 0;
    uint64_t total_on_ms = 0;
    bool on = false;
    for (size_t i = 1; i < motor.count; ++i) {
        const write_record_t record = motor.records[i];
        assert(record.accepted);
        if (record.strength) {
            assert(!on);
            if (i > 1) {
                assert(record.time_ms - off_since >= 65);
            }
            on_since = record.time_ms;
            on = true;
        } else {
            assert(on && record.time_ms - on_since <= 65);
            total_on_ms += record.time_ms - on_since;
            off_since = record.time_ms;
            on = false;
        }
    }
    assert(!on && total_on_ms < 30000);
    const size_t writes = motor.count;
    assert(poll(&state, &motor, base + 200000) == HAPTICS_OK);
    assert(motor.count == writes);
}

static void test_faults_stop_and_require_explicit_recovery(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 0);
    motor.fail_on = 1;
    motor.change_before_failure = true; /* A failed write may still energize hardware. */
    assert(request(&state, &motor, HAPTICS_LOCK, 0) == HAPTICS_HARDWARE_ERROR);
    assert(motor.output == 0 && !haptics_available(&state));
    assert(!haptics_enabled(&state));
    assert(enable(&state, &motor, true, 500) == HAPTICS_HARDWARE_ERROR);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1000) == HAPTICS_HARDWARE_ERROR);
    motor.time_ms = 1000;
    assert(haptics_recover(&state, 1000) == HAPTICS_OK);
    assert(haptics_available(&state) && !haptics_enabled(&state));
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1000) == HAPTICS_DISABLED);
    assert(enable(&state, &motor, true, 1000) == HAPTICS_OK);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 1000) == HAPTICS_OK);

    motor.change_before_failure = false;
    motor.fail_off = 3;
    assert(poll(&state, &motor, 1024) == HAPTICS_HARDWARE_ERROR);
    assert(motor.output > 0 && haptics_active(&state));
    assert(!haptics_available(&state));
    const size_t failed_writes = motor.count;
    for (uint64_t time_ms = 1025; time_ms < 1044; ++time_ms) {
        assert(enable(&state, &motor, false, time_ms) == HAPTICS_HARDWARE_ERROR);
        assert(poll(&state, &motor, time_ms) == HAPTICS_HARDWARE_ERROR);
    }
    assert(motor.count == failed_writes);
    assert(poll(&state, &motor, 1044) == HAPTICS_HARDWARE_ERROR);
    assert(motor.output > 0);
    assert(poll(&state, &motor, 1064) == HAPTICS_HARDWARE_ERROR);
    assert(motor.output == 0 && !haptics_active(&state));
    assert(!haptics_available(&state));

    motor.fail_off = 2;
    motor.time_ms = 1100;
    assert(haptics_recover(&state, 1100) == HAPTICS_HARDWARE_ERROR);
    assert(!haptics_available(&state) && !haptics_enabled(&state));
}

static void test_time_failures_and_integer_boundary(void)
{
    haptics_t state;
    motor_t motor;
    initialize(&state, &motor, 100);
    assert(request(&state, &motor, HAPTICS_LOCK, 100) == HAPTICS_OK);
    assert(poll(&state, &motor, 99) == HAPTICS_TIME_ERROR);
    assert(motor.output == 0 && !haptics_available(&state));
    assert(enable(&state, &motor, true, 1000) == HAPTICS_TIME_ERROR);
    motor.time_ms = 0;
    assert(haptics_recover(&state, 0) == HAPTICS_OK);
    assert(enable(&state, &motor, true, 0) == HAPTICS_OK);
    assert(request(&state, &motor, HAPTICS_CONFIRM, 0) == HAPTICS_OK);

    initialize(&state, &motor, UINT64_MAX - 1000);
    const uint64_t last_safe_start = UINT64_MAX - 224;
    assert(request(&state, &motor, HAPTICS_CONFIRM, last_safe_start) == HAPTICS_OK);
    assert(poll(&state, &motor, last_safe_start + 24) == HAPTICS_OK);
    assert(motor.output == 0);
    assert(request(&state, &motor, HAPTICS_CONFIRM, UINT64_MAX) == HAPTICS_TIME_ERROR);
    assert(motor.output == 0 && !haptics_available(&state));

    memset(&motor, 0, sizeof(motor));
    motor.time_ms = UINT64_MAX;
    motor.fail_off = 5;
    assert(haptics_init(&state, true, write_motor, &motor, UINT64_MAX) == HAPTICS_HARDWARE_ERROR);
    const size_t writes = motor.count;
    for (unsigned i = 0; i < 100; ++i) {
        assert(poll(&state, &motor, UINT64_MAX) == HAPTICS_HARDWARE_ERROR);
    }
    assert(motor.count == writes); /* Retry arithmetic must not wrap or spin. */
}

int main(void)
{
    test_atomic_dispatch_cancellation();
    test_dispatch_failures_and_generation_exhaustion();
    test_required_motor_evidence();
    test_pulse_boundaries_and_strength();
    test_distinct_error_and_late_poll();
    test_flood_does_not_extend_or_queue();
    test_disable_cancels_without_replay();
    test_continuous_operation_across_32_bit_time();
    test_faults_stop_and_require_explicit_recovery();
    test_time_failures_and_integer_boundary();
    puts("haptics: all tests passed");
    return 0;
}
