// SPDX-License-Identifier: Apache-2.0
#include "board_sensor_journal.h"

_Static_assert(sizeof(board_sensor_record_t) == BOARD_SENSOR_RECORD_BYTES, "Sensor record layout must be 88 bytes");
_Static_assert(offsetof(board_sensor_record_t, kind) == 40, "Sensor kind offset");
_Static_assert(offsetof(board_sensor_record_t, error) == 44, "Sensor error offset");
_Static_assert(offsetof(board_sensor_record_t, payload) == 56, "Sensor payload offset");

bool board_sensor_journal_init(board_sensor_journal_t *journal, board_sensor_record_t *records, size_t capacity)
{
    if (!journal || !records || !capacity || capacity > BOARD_SENSOR_JOURNAL_CAPACITY || journal->records) return false;
    *journal = (board_sensor_journal_t){.records = records, .capacity = capacity, .state = BOARD_SENSOR_JOURNAL_LIVE};
    return true;
}

bool board_sensor_journal_append(board_sensor_journal_t *journal, const board_sensor_record_t *record)
{
    if (!journal || !record || journal->state != BOARD_SENSOR_JOURNAL_LIVE || record->sequence ||
            !journal->records || !journal->capacity) return false;
    if (journal->latest_sequence == UINT64_MAX) {
        journal->state = BOARD_SENSOR_JOURNAL_EXHAUSTED;
        return false;
    }
    size_t index;
    if (journal->count == journal->capacity) {
        index = journal->head;
        journal->head = (journal->head + 1U) % journal->capacity;
    } else {
        index = (journal->head + journal->count) % journal->capacity;
        ++journal->count;
    }
    journal->records[index] = *record;
    journal->records[index].sequence = ++journal->latest_sequence;
    if (journal->latest_sequence == UINT64_MAX) journal->state = BOARD_SENSOR_JOURNAL_EXHAUSTED;
    return true;
}

bool board_sensor_journal_bounds(const board_sensor_journal_t *journal, uint64_t observed_us, board_sensor_batch_t *batch)
{
    if (!journal || !batch) return false;
    *batch = (board_sensor_batch_t){
        .oldest_sequence = journal->count ? journal->records[journal->head].sequence : 0,
        .latest_sequence = journal->latest_sequence, .next_after = journal->latest_sequence,
        .observed_us = observed_us, .journal_state = journal->state,
        .state = journal->state == BOARD_SENSOR_JOURNAL_UNAVAILABLE ? BOARD_SENSOR_READ_UNAVAILABLE : BOARD_SENSOR_READ_EMPTY,
    };
    return true;
}

bool board_sensor_journal_copy(const board_sensor_journal_t *journal, uint64_t after,
    board_sensor_record_t *records, size_t capacity, uint64_t observed_us, board_sensor_batch_t *batch)
{
    if (!journal || !records || !batch || !capacity || capacity > BOARD_SENSOR_JOURNAL_READ_MAX ||
            after > journal->latest_sequence) return false;
    (void)board_sensor_journal_bounds(journal, observed_us, batch);
    batch->next_after = after;
    if (!journal->count) return true;
    uint64_t retained_after = after;
    if (after < batch->oldest_sequence - 1U) {
        batch->state = BOARD_SENSOR_READ_GAP;
        batch->lost_records = batch->oldest_sequence - 1U - after;
        retained_after = batch->oldest_sequence - 1U;
    }
    if (retained_after < journal->latest_sequence) {
        if (!batch->lost_records) batch->state = BOARD_SENSOR_READ_DATA;
        /* Bounds, loss and retained records belong to the same locked copy.
         * A gap-only resume at oldest-1 would already be stale on the next
         * request whenever the full ring advances between reads. */
        const size_t offset = (size_t)(retained_after + 1U - batch->oldest_sequence);
        const size_t remaining = journal->count - offset;
        batch->count = (uint32_t)(remaining < capacity ? remaining : capacity);
        for (uint32_t i = 0; i < batch->count; ++i) records[i] = journal->records[(journal->head + offset + i) % journal->capacity];
        batch->next_after = records[batch->count - 1U].sequence;
    }
    return true;
}

bool board_sensor_input_valid(const board_sensor_record_t *record)
{
    const uint16_t allowed = BOARD_SENSOR_FLAG_SAMPLE_TIME | BOARD_SENSOR_FLAG_SOURCE_SEQ | BOARD_SENSOR_FLAG_REDACTED;
    if (!record || record->sequence || record->observed_us || record->source_epoch || record->error ||
            record->discontinuities ||
            (record->flags & ~allowed) || !(record->flags & BOARD_SENSOR_FLAG_SAMPLE_TIME) || !record->sample_us ||
            ((record->flags & BOARD_SENSOR_FLAG_SOURCE_SEQ) ? !record->source_sequence : record->source_sequence != 0)) return false;
    if (record->flags & BOARD_SENSOR_FLAG_REDACTED) {
        if (record->kind != BOARD_SENSOR_TOUCH || record->validity) return false;
        for (size_t i = 0; i < 8; ++i) if (record->payload[i]) return false;
        return true;
    }
    if (record->validity != BOARD_SENSOR_VALID_INPUT) return false;
    if (record->kind == BOARD_SENSOR_BUTTON) {
        if (record->payload[0] > 1) return false;
        for (size_t i = 1; i < 8; ++i) if (record->payload[i]) return false;
        return true;
    }
    if (record->kind != BOARD_SENSOR_TOUCH || record->payload[0] > 2 || record->payload[1] > 3) return false;
    const uint32_t count = record->payload[0];
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t *point = &record->payload[2 + i * 3];
        if (i >= count) {
            if (point[0] || point[1] || point[2]) return false;
        } else if (point[0] > UINT8_MAX || point[1] > UINT16_MAX || point[2] > UINT16_MAX) return false;
    }
    return true;
}
