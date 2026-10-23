// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable types only: no SDK, LVGL, allocation or sensor dependency. */
#define BOARD_IMU_STREAM_CAPACITY 8U
#define BOARD_IMU_STREAM_MAX_AGE_US UINT64_C(50000)

typedef struct {
    float x_dps, y_dps, z_dps;
    float x_g, y_g, z_g;
    uint64_t timestamp_us;
} board_imu_sample_t;

typedef uint64_t board_imu_consumer_t;

typedef struct {
    board_imu_sample_t sample;
    uint64_t sequence;
    uint64_t delivery_epoch;
} board_imu_frame_t;

typedef enum {
    BOARD_IMU_DELIVERY_DATA = 0,
    BOARD_IMU_DELIVERY_EMPTY = 1,
    BOARD_IMU_DELIVERY_DISCONTINUITY = 2,
    BOARD_IMU_DELIVERY_UNAVAILABLE = 3,
    BOARD_IMU_DELIVERY_FAILED = 4,
} board_imu_delivery_state_t;

/* Bitmask. Intentional boundaries are distinct from source/delivery loss. */
enum {
    BOARD_IMU_DISCONTINUITY_ATTACH = 1U << 0,
    BOARD_IMU_DISCONTINUITY_FENCE = 1U << 1,
    BOARD_IMU_DISCONTINUITY_OVERFLOW = 1U << 2,
    BOARD_IMU_DISCONTINUITY_STALE = 1U << 3,
    BOARD_IMU_DISCONTINUITY_SOURCE = 1U << 4,
    BOARD_IMU_DISCONTINUITY_REJECTED = 1U << 5,
    BOARD_IMU_DISCONTINUITY_CLOCK = 1U << 6,
    BOARD_IMU_DISCONTINUITY_ORDER = 1U << 7,
    BOARD_IMU_DISCONTINUITY_ID_EXHAUSTED = 1U << 8,
};

typedef struct {
    board_imu_delivery_state_t state;
    uint32_t count;
    uint32_t discontinuities;
    uint64_t delivery_epoch;
    uint64_t latest_sequence; /* Latest accepted publication, including loss. */
    uint64_t dropped_frames; /* Lifetime accepted frames canceled/lost while attached; saturates. */
    bool dropped_frames_saturated;
    int32_t source_error; /* ESP error domain, independently of delivery state. */
} board_imu_batch_t;

#ifdef __cplusplus
}
#endif
