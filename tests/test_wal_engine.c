#include <stdio.h>
#include <string.h>

#include "tsdb/record.h"
#include "tsdb/wal.h"
#include "tsdb/wal_format.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

#define WAL_FILE "wal_engine_test.bin"
#define BACKUP_ONE "wal_engine_backup_1.bin"
#define BACKUP_TWO "wal_engine_backup_2.bin"

typedef struct {
    size_t seen;
} ReplayState;

static TsdbStatus visit_batch(const TsdbPoint *points, size_t count, uint64_t sequence,
                              void *context)
{
    ReplayState *state = (ReplayState *)context;
    if (sequence != state->seen + 1u) {
        return TSDB_ERR_FORMAT;
    }
    if (sequence == 1u) {
        if (count != 2u || strcmp(points[0].metric, "cpu") != 0 ||
            points[0].timestamp_ms != 1234 || points[0].value != 42.5 ||
            strcmp(points[1].metric, "ram") != 0 || points[1].timestamp_ms != 2000 ||
            points[1].value != 67.0) {
            return TSDB_ERR_FORMAT;
        }
    } else if (sequence == 2u || sequence == 3u) {
        if (count != 1u || strcmp(points[0].metric, "cpu") != 0 ||
            points[0].timestamp_ms != (int64_t)(sequence * 1000u) ||
            points[0].value != (double)sequence) {
            return TSDB_ERR_FORMAT;
        }
    } else {
        return TSDB_ERR_FORMAT;
    }
    ++state->seen;
    return TSDB_OK;
}

static long file_length(const char *path)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb") != 0) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (fclose(file) != 0) {
        return -1;
    }
    return length;
}

int main(void)
{
    (void)remove(WAL_FILE);
    (void)remove(BACKUP_ONE);
    (void)remove(BACKUP_TWO);
    TsdbWal *wal = NULL;
    CHECK(tsdb_wal_create(WAL_FILE, &wal) == TSDB_OK);
    CHECK(wal != NULL);

    ReplayState state = {0};
    size_t batches = 99u;
    CHECK(tsdb_wal_replay(wal, visit_batch, &state, &batches) == TSDB_OK);
    CHECK(batches == 0u && state.seen == 0u);

    TsdbPoint first[2] = {{{0}, 1234, 42.5}, {{0}, 2000, 67.0}};
    memcpy(first[0].metric, "cpu", sizeof "cpu");
    memcpy(first[1].metric, "ram", sizeof "ram");
    TsdbPoint second = {{0}, 2000, 2.0};
    memcpy(second.metric, "cpu", sizeof "cpu");
    TsdbPoint third = {{0}, 3000, 3.0};
    memcpy(third.metric, "cpu", sizeof "cpu");

    TsdbPoint invalid = {{0}, 0, 1.0};
    memcpy(invalid.metric, "bad!", sizeof "bad!");
    uint64_t sequence = 99u;
    CHECK(tsdb_wal_write_batch(wal, &invalid, 1u, &sequence) == TSDB_ERR_ARGUMENT);
    CHECK(sequence == 99u);
    CHECK(tsdb_wal_write_batch(wal, first, 0u, &sequence) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_wal_write_batch(wal, first, TSDB_WAL_MAX_BATCH_POINTS + 1u, &sequence) ==
          TSDB_ERR_LIMIT);

    CHECK(tsdb_wal_write_batch(wal, first, 2u, &sequence) == TSDB_OK);
    CHECK(sequence == 1u);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, &sequence) == TSDB_OK);
    CHECK(sequence == 2u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;

    const long first_size =
        TSDB_WAL_BEGIN_V2_SIZE + 2u * TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE;
    const long second_size = TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE;
    const long committed_size = TSDB_WAL_FILE_HEADER_SIZE + first_size + second_size;
    CHECK(file_length(WAL_FILE) == committed_size);

    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_WRITE, &wal) == TSDB_OK);
    state.seen = 0u;
    CHECK(tsdb_wal_replay(wal, visit_batch, &state, &batches) == TSDB_OK);
    CHECK(batches == 2u && state.seen == 2u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;

    uint8_t begin[TSDB_WAL_BEGIN_V2_SIZE] = {0};
    CHECK(tsdb_wal_begin_v2_encode(1u, 3u, begin) == TSDB_OK);
    FILE *file = NULL;
    CHECK(fopen_s(&file, WAL_FILE, "ab") == 0);
    CHECK(fwrite(begin, 1u, 7u, file) == 7u);
    CHECK(fclose(file) == 0);
    CHECK(file_length(WAL_FILE) == committed_size + 7);

    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_ERR_INCOMPLETE);
    CHECK(wal == NULL && file_length(WAL_FILE) == committed_size + 7);
    CHECK(fopen_s(&file, BACKUP_ONE, "wb") == 0);
    CHECK(fwrite("keep", 1u, 4u, file) == 4u);
    CHECK(fclose(file) == 0);
    uint64_t discarded = 0u;
    CHECK(tsdb_wal_repair(WAL_FILE, BACKUP_ONE, &discarded) == TSDB_ERR_EXISTS);
    CHECK(file_length(WAL_FILE) == committed_size + 7);
    CHECK(file_length(BACKUP_ONE) == 4);
    CHECK(remove(BACKUP_ONE) == 0);
    CHECK(tsdb_wal_repair(WAL_FILE, BACKUP_ONE, &discarded) == TSDB_OK);
    CHECK(discarded == 7u && file_length(BACKUP_ONE) == committed_size + 7);
    CHECK(file_length(WAL_FILE) == committed_size);
    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_WRITE, &wal) == TSDB_OK);
    state.seen = 0u;
    CHECK(tsdb_wal_replay(wal, visit_batch, &state, &batches) == TSDB_OK);
    CHECK(batches == 2u && state.seen == 2u);
    CHECK(tsdb_wal_write_batch(wal, &third, 1u, &sequence) == TSDB_OK);
    CHECK(sequence == 3u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;

    const long three_size = committed_size + second_size;
    CHECK(file_length(WAL_FILE) == three_size);
    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_OK);
    state.seen = 0u;
    CHECK(tsdb_wal_replay(wal, visit_batch, &state, &batches) == TSDB_OK);
    CHECK(batches == 3u && state.seen == 3u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;

    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    uint8_t partial_commit[TSDB_WAL_COMMIT_SIZE] = {0};
    CHECK(tsdb_wal_begin_v2_encode(1u, 4u, begin) == TSDB_OK);
    CHECK(tsdb_record_encode(&third, raw) == TSDB_OK);
    CHECK(tsdb_wal_commit_encode(4u, 0u, partial_commit) == TSDB_OK);
    CHECK(fopen_s(&file, WAL_FILE, "ab") == 0);
    CHECK(fwrite(begin, 1u, sizeof begin, file) == sizeof begin);
    CHECK(fwrite(raw, 1u, sizeof raw, file) == sizeof raw);
    CHECK(fwrite(partial_commit, 1u, 8u, file) == 8u);
    CHECK(fclose(file) == 0);
    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_ERR_INCOMPLETE);
    const long partial_size = (long)(TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE + 8u);
    CHECK(wal == NULL && file_length(WAL_FILE) == three_size + partial_size);
    CHECK(tsdb_wal_repair(WAL_FILE, BACKUP_TWO, &discarded) == TSDB_OK);
    CHECK(discarded == TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE + 8);
    CHECK(file_length(BACKUP_TWO) == three_size + (long)discarded);
    CHECK(file_length(WAL_FILE) == three_size);
    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_OK);
    state.seen = 0u;
    CHECK(tsdb_wal_replay(wal, visit_batch, &state, &batches) == TSDB_OK);
    CHECK(batches == 3u && state.seen == 3u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;

    CHECK(fopen_s(&file, WAL_FILE, "r+b") == 0);
    CHECK(fseek(file, TSDB_WAL_FILE_HEADER_SIZE + TSDB_WAL_BEGIN_V2_SIZE + 16, SEEK_SET) == 0);
    int old_byte = fgetc(file);
    CHECK(old_byte != EOF);
    CHECK(fseek(file, TSDB_WAL_FILE_HEADER_SIZE + TSDB_WAL_BEGIN_V2_SIZE + 16, SEEK_SET) == 0);
    CHECK(fputc(old_byte ^ 1, file) != EOF);
    CHECK(fclose(file) == 0);
    CHECK(tsdb_wal_open(WAL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_ERR_CHECKSUM);
    CHECK(wal == NULL && file_length(WAL_FILE) == three_size);

    CHECK(remove(WAL_FILE) == 0);
    CHECK(remove(BACKUP_ONE) == 0);
    CHECK(remove(BACKUP_TWO) == 0);
    puts("WAL engine tests passed.");
    return 0;
}
