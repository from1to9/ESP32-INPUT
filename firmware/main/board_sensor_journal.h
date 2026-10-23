// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_SENSOR_JOURNAL_CAPACITY 256U
#define BOARD_SENSOR_JOURNAL_READ_MAX 8U
#define BOARD_SENSOR_RECORD_BYTES 88U

typedef enum {
    BOARD_SENSOR_IMU_SAMPLE = 1,
    BOARD_SENSOR_IMU_REJECTED = 2,
    BOARD_SENSOR_POWER = 3,
    BOARD_SENSOR_TOUCH = 4,
    BOARD_SENSOR_BUTTON = 5,
} board_sensor_kind_t;

enum {
    BOARD_SENSOR_FLAG_SOURCE_SEQ = 1U << 0,
    BOARD_SENSOR_FLAG_SOURCE_EPOCH = 1U << 1,
    BOARD_SENSOR_FLAG_SAMPLE_TIME = 1U << 2,
    BOARD_SENSOR_FLAG_OBSERVED_TIME = 1U << 3,
    BOARD_SENSOR_FLAG_DRDY = 1U << 4,
    BOARD_SENSOR_FLAG_REDACTED = 1U << 5, /* TOUCH only: validity/payload are all zero. */
    /* The raw BMI270 status byte occupies bits 8..15, only when DRDY is set. */
    BOARD_SENSOR_FLAG_STATUS_SHIFT = 8,
    BOARD_SENSOR_VALID_IMU_AXES = 0x3fU,
    BOARD_SENSOR_VALID_READ_START = 1U << 6,
    BOARD_SENSOR_VALID_READ_END = 1U << 7,
    BOARD_SENSOR_VALID_DURATION = 1U << 8,
    BOARD_SENSOR_VALID_SOURCE_ERROR = 1U << 9,
    BOARD_SENSOR_VALID_POWER_PERCENT = 1U << 0,
    BOARD_SENSOR_VALID_POWER_VOLTAGE = 1U << 1,
    BOARD_SENSOR_VALID_POWER_CURRENT = 1U << 2,
    BOARD_SENSOR_VALID_POWER_TEMPERATURE = 1U << 3,
    BOARD_SENSOR_VALID_POWER_STATE = 1U << 4,
    BOARD_SENSOR_VALID_INPUT = 1U << 0,
};

typedef enum {
    BOARD_SENSOR_ATTEMPT_NOT_READY = 1,
    BOARD_SENSOR_ATTEMPT_LATE = 2,
    BOARD_SENSOR_ATTEMPT_COMMUNICATION = 3,
    BOARD_SENSOR_ATTEMPT_FATAL = 4,
    BOARD_SENSOR_ATTEMPT_PUBLICATION = 5,
    BOARD_SENSOR_ATTEMPT_STARTUP = 6,
} board_sensor_attempt_t;

typedef enum {
    BOARD_SENSOR_STAGE_NONE = 0,
    BOARD_SENSOR_STAGE_HANDLE = 1,
    BOARD_SENSOR_STAGE_CONFIG = 2,
    BOARD_SENSOR_STAGE_FRAME = 3,
    BOARD_SENSOR_STAGE_DATA_READY = 4,
    BOARD_SENSOR_STAGE_BATCH_TIME = 5,
    BOARD_SENSOR_STAGE_FINITE = 6,
    BOARD_SENSOR_STAGE_SAMPLE = 7,
    BOARD_SENSOR_STAGE_CLOCK = 8,
    BOARD_SENSOR_STAGE_PUBLICATION_CLOCK = 9,
    BOARD_SENSOR_STAGE_PUBLICATION_AGE = 10,
    BOARD_SENSOR_STAGE_SAMPLE_TIME = 11,
    BOARD_SENSOR_STAGE_GENERATION = 12,
    BOARD_SENSOR_STAGE_FIRST_SAMPLE = 13,
    BOARD_SENSOR_STAGE_INIT = 14,
    BOARD_SENSOR_STAGE_START = 15,
} board_sensor_stage_t;

/* Native layout matches the 88-byte v1 field offsets below. The API owner
 * encodes each field explicitly little-endian; never transmit struct padding.
 * 0 global sequence; 8 source sequence; 16 hardware generation; 24 sample_us;
 * 32 publication/observation_us; 40 kind:u16; 42 flags:u16; 44 error:i32;
 * 48 discontinuities:u32; 52 validity:u32; 56 typed payload[32].
 * IMU sample payload: 6 IEEE-754 float32 bits (gx,gy,gz dps; ax,ay,az g),
 * then acquisition-start u64 in words 6(low)/7(high). sample_us is the existing
 * acquisition completion timestamp, never a known conversion/ODR timestamp.
 * IMU rejected payload: start/end/duration u64 at 0/8/16; bytes24 DRDY-known,
 * 25 status, 26 attempt enum, 27 stage enum; source cache error:i32 at28.
 * Only marked known values are meaningful. Rejected records have no axes and
 * must never be replayed as zero or accepted samples. Discontinuities use the
 * board_imu_stream.h reason bits, excluding observer-controlled attach/fence.
 * Power payload words: tempC bits, signed mA bits, mV, percent, gauge bool,
 * truth enums packed battery/external/charging/full in successive 2-bit pairs,
 * then two reserved zero words. Temperature is BQ27220 register0x06 /10-273.15.
 * Touch payload words: count, valid(bit0)|overflow(bit1), id0,x0,y0,id1,x1,y1.
 * Button payload words: edge(0 down/1 up), then seven reserved zero words.
 * Input timestamps originate in existing owners; source_epoch is unknown for
 * input and power, never a coordinator permission epoch. No read performs I/O. */
typedef struct {
    uint64_t sequence, source_sequence, source_epoch, sample_us, observed_us;
    uint16_t kind, flags;
    int32_t error;
    uint32_t discontinuities, validity;
    uint32_t payload[8];
} board_sensor_record_t;

typedef enum {
    BOARD_SENSOR_JOURNAL_UNAVAILABLE = 0,
    BOARD_SENSOR_JOURNAL_LIVE = 1,
    BOARD_SENSOR_JOURNAL_EXHAUSTED = 2,
} board_sensor_journal_state_t;
typedef enum {
    BOARD_SENSOR_READ_DATA = 0,
    BOARD_SENSOR_READ_EMPTY = 1,
    BOARD_SENSOR_READ_GAP = 2,
    BOARD_SENSOR_READ_UNAVAILABLE = 3,
} board_sensor_read_state_t;
typedef struct {
    uint64_t oldest_sequence, latest_sequence, next_after, lost_records, observed_us;
    uint32_t count;
    board_sensor_read_state_t state;
    board_sensor_journal_state_t journal_state;
} board_sensor_batch_t;

/* Portable private ring. Caller provides lifetime storage, serializes all
 * operations and initializes exactly once. No reader consumes or mutates it. */
typedef struct {
    board_sensor_record_t *records;
    size_t capacity, head, count;
    uint64_t latest_sequence;
    board_sensor_journal_state_t state;
} board_sensor_journal_t;
bool board_sensor_journal_init(board_sensor_journal_t *, board_sensor_record_t *, size_t);
bool board_sensor_journal_append(board_sensor_journal_t *, const board_sensor_record_t *);
bool board_sensor_journal_copy(const board_sensor_journal_t *, uint64_t after,
    board_sensor_record_t *, size_t capacity, uint64_t observed_us, board_sensor_batch_t *);
bool board_sensor_journal_bounds(const board_sensor_journal_t *, uint64_t observed_us, board_sensor_batch_t *);
bool board_sensor_input_valid(const board_sensor_record_t *);

#ifdef __cplusplus
}
#endif
