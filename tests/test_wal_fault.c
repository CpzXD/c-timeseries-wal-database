#include <stdio.h>
#include <string.h>

#include "tsdb/wal.h"
#include "tsdb/wal_format.h"
#include "wal_fault_hooks.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

#define WRITE_FILE "wal_fault_write.bin"
#define FLUSH_FILE "wal_fault_flush.bin"
#define TAIL_FILE "wal_fault_tail.bin"
#define TAIL_BACKUP "wal_fault_tail_backup.bin"
#define UNCERTAIN_FILE "wal_fault_uncertain.bin"

static long file_length(const char *path)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb") != 0) {
        return -1;
    }
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return length;
}

static TsdbPoint make_point(uint64_t sequence)
{
    TsdbPoint point = {{0}, (int64_t)(sequence * 1000u), (double)sequence};
    memcpy(point.metric, "cpu", sizeof "cpu");
    return point;
}

static int create_with_baseline(const char *path, TsdbWal **wal)
{
    TsdbPoint first = make_point(1u);
    uint64_t sequence = 0u;
    return tsdb_wal_create(path, wal) == TSDB_OK &&
           tsdb_wal_write_batch(*wal, &first, 1u, &sequence) == TSDB_OK && sequence == 1u;
}

typedef struct {
    size_t seen;
} ReplayState;

static TsdbStatus visit_batch(const TsdbPoint *points, size_t count, uint64_t sequence,
                              void *context)
{
    ReplayState *state = (ReplayState *)context;
    if (count != 1u || sequence != state->seen + 1u || strcmp(points[0].metric, "cpu") != 0 ||
        points[0].timestamp_ms != (int64_t)(sequence * 1000u) ||
        points[0].value != (double)sequence) {
        return TSDB_ERR_FORMAT;
    }
    ++state->seen;
    return TSDB_OK;
}

static int replay_has(const char *path, size_t expected)
{
    TsdbWal *wal = NULL;
    if (tsdb_wal_open(path, TSDB_WAL_READ_ONLY, &wal) != TSDB_OK) {
        return 0;
    }
    ReplayState state = {0};
    size_t batches = 0u;
    TsdbStatus status = tsdb_wal_replay(wal, visit_batch, &state, &batches);
    TsdbStatus close_status = tsdb_wal_close(wal);
    return status == TSDB_OK && close_status == TSDB_OK && batches == expected &&
           state.seen == expected;
}

int main(void)
{
    (void)remove(WRITE_FILE);
    (void)remove(FLUSH_FILE);
    (void)remove(TAIL_FILE);
    (void)remove(TAIL_BACKUP);
    (void)remove(UNCERTAIN_FILE);

    const long batch_size =
        (long)(TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE);
    const long committed_size = (long)TSDB_WAL_FILE_HEADER_SIZE + batch_size;
    TsdbPoint second = make_point(2u);
    TsdbWal *wal = NULL;
    uint64_t sequence = 99u;

    CHECK(create_with_baseline(WRITE_FILE, &wal));
    tsdb_wal_test_fail_write(wal, 7u, 0);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, &sequence) == TSDB_ERR_IO);
    CHECK(sequence == 99u);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, NULL) == TSDB_ERR_IO);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;
    CHECK(file_length(WRITE_FILE) == committed_size);
    CHECK(replay_has(WRITE_FILE, 1u));
    CHECK(tsdb_wal_open(WRITE_FILE, TSDB_WAL_READ_WRITE, &wal) == TSDB_OK);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, &sequence) == TSDB_OK);
    CHECK(sequence == 2u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;
    CHECK(replay_has(WRITE_FILE, 2u));

    CHECK(create_with_baseline(FLUSH_FILE, &wal));
    tsdb_wal_test_fail_flush(wal, 0);
    sequence = 99u;
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, &sequence) == TSDB_ERR_IO);
    CHECK(sequence == 99u);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;
    CHECK(file_length(FLUSH_FILE) == committed_size);
    CHECK(replay_has(FLUSH_FILE, 1u));

    CHECK(create_with_baseline(TAIL_FILE, &wal));
    tsdb_wal_test_fail_write(wal, 7u, 1);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, NULL) == TSDB_ERR_IO);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;
    CHECK(file_length(TAIL_FILE) == committed_size + 7);
    CHECK(tsdb_wal_open(TAIL_FILE, TSDB_WAL_READ_ONLY, &wal) == TSDB_ERR_INCOMPLETE);
    CHECK(wal == NULL && file_length(TAIL_FILE) == committed_size + 7);
    uint64_t discarded = 0u;
    CHECK(tsdb_wal_repair(TAIL_FILE, TAIL_BACKUP, &discarded) == TSDB_OK);
    CHECK(discarded == 7u && file_length(TAIL_BACKUP) == committed_size + 7);
    CHECK(file_length(TAIL_FILE) == committed_size);
    CHECK(replay_has(TAIL_FILE, 1u));

    CHECK(create_with_baseline(UNCERTAIN_FILE, &wal));
    tsdb_wal_test_fail_flush(wal, 1);
    CHECK(tsdb_wal_write_batch(wal, &second, 1u, NULL) == TSDB_ERR_IO);
    CHECK(tsdb_wal_close(wal) == TSDB_OK);
    wal = NULL;
    CHECK(file_length(UNCERTAIN_FILE) == committed_size + batch_size);
    CHECK(replay_has(UNCERTAIN_FILE, 2u));

    CHECK(remove(WRITE_FILE) == 0);
    CHECK(remove(FLUSH_FILE) == 0);
    CHECK(remove(TAIL_FILE) == 0);
    CHECK(remove(TAIL_BACKUP) == 0);
    CHECK(remove(UNCERTAIN_FILE) == 0);
    puts("WAL fault tests passed.");
    return 0;
}
