// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_sensor_journal.h"

static board_sensor_record_t storage[BOARD_SENSOR_JOURNAL_CAPACITY];
static board_sensor_record_t out[BOARD_SENSOR_JOURNAL_READ_MAX];
static board_sensor_batch_t batch;

static board_sensor_record_t observation(uint64_t source_sequence)
{
    return (board_sensor_record_t){.source_sequence = source_sequence, .source_epoch = 7,
        .sample_us = source_sequence * 10000, .observed_us = source_sequence * 10000 + 100,
        .kind = BOARD_SENSOR_IMU_SAMPLE,
        .flags = BOARD_SENSOR_FLAG_SOURCE_SEQ | BOARD_SENSOR_FLAG_SOURCE_EPOCH |
            BOARD_SENSOR_FLAG_SAMPLE_TIME | BOARD_SENSOR_FLAG_OBSERVED_TIME,
        .validity = BOARD_SENSOR_VALID_IMU_AXES | BOARD_SENSOR_VALID_READ_START,
        .payload = {0x80000000, 0x3fa00000, 0xbf800000, 0, 0, 0x3f800000, 1234, 0},
    };
}

static void test_preservation_rejection_and_independent_clients(void)
{
    board_sensor_journal_t journal = {0};
    assert(board_sensor_journal_init(&journal, storage, 256));
    for (uint64_t i = 1; i <= 32; ++i) {
        board_sensor_record_t record = observation(i);
        if (i == 16) {
            record.kind = BOARD_SENSOR_IMU_REJECTED;
            record.source_sequence = 0;
            record.error = -9;
            record.discontinuities = 32;
        }
        assert(board_sensor_journal_append(&journal, &record));
    }
    uint64_t first = 0, second = 0;
    for (unsigned pass = 0; pass < 4; ++pass) {
        const board_sensor_journal_t before = journal;
        assert(board_sensor_journal_copy(&journal, first, out, 8, 1000000, &batch));
        assert(batch.state == BOARD_SENSOR_READ_DATA && batch.count == 8 && !batch.lost_records);
        for (unsigned i = 0; i < 8; ++i) {
            assert(out[i].sequence == first + i + 1);
            if (out[i].sequence != 16) assert(out[i].payload[0] == 0x80000000 && out[i].source_sequence == out[i].sequence);
            else assert(out[i].kind == BOARD_SENSOR_IMU_REJECTED && out[i].error == -9);
        }
        first = batch.next_after;
        assert(memcmp(&before, &journal, sizeof(journal)) == 0);
    }
    for (unsigned pass = 0; pass < 32; ++pass) {
        assert(board_sensor_journal_copy(&journal, second, out, 1, 1000000, &batch));
        assert(batch.count == 1 && out[0].sequence == second + 1);
        second = batch.next_after;
    }
    assert(first == 32 && second == 32 && journal.count == 32);
}

static void test_overwrite_gap_future_and_output_integrity(void)
{
    board_sensor_journal_t journal = {0};
    assert(board_sensor_journal_init(&journal, storage, 4));
    for (uint64_t i = 1; i <= 12; ++i) {
        const board_sensor_record_t record = observation(i);
        assert(board_sensor_journal_append(&journal, &record));
    }
    memset(out, 0x5a, sizeof(out));
    const board_sensor_record_t sentinel = out[4];
    assert(board_sensor_journal_copy(&journal, 3, out, 8, 999, &batch));
    assert(batch.state == BOARD_SENSOR_READ_GAP && batch.count == 4 && batch.oldest_sequence == 9 &&
        batch.latest_sequence == 12 && batch.lost_records == 5 && batch.next_after == 12);
    assert(memcmp(&out[4], &sentinel, sizeof(sentinel)) == 0);
    assert(out[0].sequence == 9 && out[3].sequence == 12);
    /* A second independent cursor reads the same retained records without
     * consuming the first reader's snapshot or the journal. */
    assert(board_sensor_journal_copy(&journal, 8, out, 8, 999, &batch));
    assert(batch.count == 4 && out[0].sequence == 9 && out[3].sequence == 12 && batch.next_after == 12);
    assert(board_sensor_journal_copy(&journal, 12, out, 8, 999, &batch));
    assert(batch.state == BOARD_SENSOR_READ_EMPTY && !batch.count);
    const board_sensor_batch_t saved = batch;
    assert(!board_sensor_journal_copy(&journal, 13, out, 8, 999, &batch));
    assert(!board_sensor_journal_copy(&journal, UINT64_MAX, out, 8, 999, &batch));
    assert(!board_sensor_journal_copy(&journal, 0, out, 0, 999, &batch));
    assert(!board_sensor_journal_copy(&journal, 0, out, 9, 999, &batch));
    assert(!board_sensor_journal_copy(&journal, 0, NULL, 8, 999, &batch));
    assert(memcmp(&saved, &batch, sizeof(saved)) == 0);
    assert(!board_sensor_journal_init(&journal, storage, 4)); /* Cannot erase retained history. */
}

static void test_empty_unavailable_and_identifier_exhaustion(void)
{
    board_sensor_journal_t journal = {0};
    assert(board_sensor_journal_copy(&journal, 0, out, 8, 10, &batch));
    assert(batch.state == BOARD_SENSOR_READ_UNAVAILABLE && batch.journal_state == BOARD_SENSOR_JOURNAL_UNAVAILABLE);
    assert(!board_sensor_journal_init(&journal, storage, 257));
    assert(board_sensor_journal_init(&journal, storage, 4));
    assert(board_sensor_journal_copy(&journal, 0, out, 8, 10, &batch));
    assert(batch.state == BOARD_SENSOR_READ_EMPTY && !batch.oldest_sequence && !batch.latest_sequence);
    journal.latest_sequence = UINT64_MAX - 2;
    const board_sensor_record_t record = observation(1);
    assert(board_sensor_journal_append(&journal, &record));
    assert(board_sensor_journal_append(&journal, &record));
    assert(journal.state == BOARD_SENSOR_JOURNAL_EXHAUSTED && journal.latest_sequence == UINT64_MAX);
    assert(!board_sensor_journal_append(&journal, &record));
    assert(board_sensor_journal_copy(&journal, 0, out, 8, 10, &batch));
    assert(batch.state == BOARD_SENSOR_READ_GAP && batch.lost_records == UINT64_MAX - 2U &&
        batch.count == 2 && batch.next_after == UINT64_MAX && out[1].sequence == UINT64_MAX);
    assert(board_sensor_journal_copy(&journal, UINT64_MAX - 2, out, 8, 10, &batch));
    assert(batch.state == BOARD_SENSOR_READ_DATA && batch.count == 2 && out[1].sequence == UINT64_MAX &&
        batch.journal_state == BOARD_SENSOR_JOURNAL_EXHAUSTED && batch.next_after == UINT64_MAX);
    assert(board_sensor_journal_copy(&journal, UINT64_MAX, out, 8, 10, &batch));
    assert(batch.state == BOARD_SENSOR_READ_EMPTY && batch.journal_state == BOARD_SENSOR_JOURNAL_EXHAUSTED);
}

static void test_advancing_full_ring_and_independent_gap_cursors(void)
{
    board_sensor_journal_t journal = {0};
    assert(board_sensor_journal_init(&journal, storage, BOARD_SENSOR_JOURNAL_CAPACITY));
    uint64_t cursor = 0, lost = 0, delivered = 0;
    for (uint64_t i = 1; i <= 300; ++i) { board_sensor_record_t r = observation(i); assert(board_sensor_journal_append(&journal, &r)); }
    for (unsigned read = 0; read < 32; ++read) {
        for (unsigned i = 0; i < 4; ++i) {
            board_sensor_record_t r = observation(journal.latest_sequence + 1U);
            assert(board_sensor_journal_append(&journal, &r));
        }
        const board_sensor_journal_t before = journal;
        assert(board_sensor_journal_copy(&journal, cursor, out, 3, 1000000, &batch));
        assert(batch.state == BOARD_SENSOR_READ_GAP && batch.count == 3 && batch.lost_records > 0);
        assert(out[0].sequence == batch.oldest_sequence && out[2].sequence == batch.next_after);
        lost += batch.lost_records; delivered += batch.count; cursor = batch.next_after;
        assert(lost + delivered == cursor && !memcmp(&before, &journal, sizeof(journal)));
        assert(board_sensor_journal_copy(&journal, 0, out, 8, 1000000, &batch));
        assert(batch.state == BOARD_SENSOR_READ_GAP && batch.count == 8 && batch.next_after > cursor &&
            batch.lost_records == batch.oldest_sequence - 1U && !memcmp(&before, &journal, sizeof(journal)));
    }
}

static void test_input_whitelist_and_redaction(void)
{
    board_sensor_record_t record = {.kind = BOARD_SENSOR_BUTTON, .flags = BOARD_SENSOR_FLAG_SAMPLE_TIME,
        .sample_us = 123, .validity = BOARD_SENSOR_VALID_INPUT};
    assert(board_sensor_input_valid(&record));
    record.payload[0] = 1; assert(board_sensor_input_valid(&record));
    record.payload[1] = 1; assert(!board_sensor_input_valid(&record)); record.payload[1] = 0;
    record.source_epoch = 9; assert(!board_sensor_input_valid(&record)); record.source_epoch = 0;
    record.sequence = 1; assert(!board_sensor_input_valid(&record)); record.sequence = 0;
    record.observed_us = 3; assert(!board_sensor_input_valid(&record)); record.observed_us = 0;
    record.kind = BOARD_SENSOR_IMU_SAMPLE; assert(!board_sensor_input_valid(&record));
    record.kind = BOARD_SENSOR_POWER; assert(!board_sensor_input_valid(&record));
    record.kind = BOARD_SENSOR_TOUCH; record.payload[0] = 2;
    record.payload[1] = 1; record.payload[2] = 7; record.payload[3] = 479; record.payload[4] = 1;
    record.payload[5] = 8; record.payload[6] = 0; record.payload[7] = 479;
    assert(board_sensor_input_valid(&record));
    record.payload[0] = 3; assert(!board_sensor_input_valid(&record)); record.payload[0] = 2;
    record.payload[5] = 256; assert(!board_sensor_input_valid(&record));
    record.flags |= BOARD_SENSOR_FLAG_REDACTED; record.validity = 0;
    assert(!board_sensor_input_valid(&record));
    memset(record.payload, 0, sizeof(record.payload)); assert(board_sensor_input_valid(&record));
    record.kind = BOARD_SENSOR_BUTTON; assert(!board_sensor_input_valid(&record));
}

int main(void)
{
    test_preservation_rejection_and_independent_clients();
    test_overwrite_gap_future_and_output_integrity();
    test_empty_unavailable_and_identifier_exhaustion();
    test_advancing_full_ring_and_independent_gap_cursors();
    test_input_whitelist_and_redaction();
    printf("Sensor journal: independent cursors, preservation, overwrite gaps, immutable reads, exhaustion and input whitelist passed; record=%zu ring=%zu bytes\n",
        sizeof(board_sensor_record_t), sizeof(storage));
    return 0;
}
