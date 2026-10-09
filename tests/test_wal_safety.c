#include <stdio.h>
#include <string.h>

#include "tsdb/crc32.h"
#include "tsdb/tsdb.h"
#include "tsdb/wal_format.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

#define V2_FILE "wal_safety_v2.bin"
#define V1_FILE "wal_safety_v1.bin"
#define SHORT_FILE "wal_safety_short.bin"
#define MISSING_FILE "wal_safety_missing.bin"
#define BACKUP_FILE "wal_safety_backup.bin"

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

static int change_byte(const char *path, long offset, int xor_mask)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "r+b") != 0) {
        return 0;
    }
    int ok = fseek(file, offset, SEEK_SET) == 0;
    int original = ok ? fgetc(file) : EOF;
    ok = ok && original != EOF && fseek(file, offset, SEEK_SET) == 0;
    if (ok) {
        ok = fputc(original ^ xor_mask, file) != EOF;
    }
    return fclose(file) == 0 && ok;
}

static int write_legacy_batch(FILE *file, uint64_t sequence, int64_t timestamp)
{
    uint8_t begin[TSDB_WAL_BEGIN_SIZE] = {0};
    uint8_t record[TSDB_RECORD_SIZE] = {0};
    uint8_t commit[TSDB_WAL_COMMIT_SIZE] = {0};
    TsdbPoint point = {{0}, timestamp, (double)timestamp};
    memcpy(point.metric, "cpu", sizeof "cpu");
    if (tsdb_wal_begin_encode(1u, sequence, begin) != TSDB_OK ||
        tsdb_record_encode(&point, record) != TSDB_OK) {
        return 0;
    }
    uint32_t state = tsdb_crc32_start();
    state = tsdb_crc32_update(state, begin, sizeof begin);
    state = tsdb_crc32_update(state, record, sizeof record);
    if (tsdb_wal_commit_encode(sequence, tsdb_crc32_finish(state), commit) != TSDB_OK) {
        return 0;
    }
    return fwrite(begin, 1u, sizeof begin, file) == sizeof begin &&
           fwrite(record, 1u, sizeof record, file) == sizeof record &&
           fwrite(commit, 1u, sizeof commit, file) == sizeof commit;
}

int main(void)
{
    (void)remove(V2_FILE);
    (void)remove(V1_FILE);
    (void)remove(SHORT_FILE);
    (void)remove(MISSING_FILE);
    (void)remove(BACKUP_FILE);

    Tsdb *db = NULL;
    CHECK(tsdb_open_readonly(MISSING_FILE, &db) == TSDB_ERR_NOT_FOUND);
    CHECK(db == NULL && file_length(MISSING_FILE) == -1);

    CHECK(tsdb_create(V2_FILE, &db) == TSDB_OK);
    TsdbPoint point = {{0}, 1, 1.0};
    memcpy(point.metric, "cpu", sizeof "cpu");
    CHECK(tsdb_write_batch(db, &point, 1u) == TSDB_OK);
    point.timestamp_ms = 2;
    point.value = 2.0;
    CHECK(tsdb_write_batch(db, &point, 1u) == TSDB_OK);
    CHECK(tsdb_close(db) == TSDB_OK);
    db = NULL;

    long v2_length = TSDB_WAL_FILE_HEADER_SIZE +
                     2u * (TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE);
    CHECK(file_length(V2_FILE) == v2_length);
    CHECK(tsdb_create(V2_FILE, &db) == TSDB_ERR_EXISTS);
    CHECK(db == NULL && file_length(V2_FILE) == v2_length);
    CHECK(change_byte(V2_FILE, TSDB_WAL_FILE_HEADER_SIZE + 4, 2));
    CHECK(tsdb_open_readonly(V2_FILE, &db) == TSDB_ERR_CHECKSUM);
    CHECK(db == NULL && file_length(V2_FILE) == v2_length);
    CHECK(tsdb_repair(V2_FILE, BACKUP_FILE, NULL) == TSDB_ERR_CHECKSUM);
    CHECK(file_length(V2_FILE) == v2_length && file_length(BACKUP_FILE) == -1);

    FILE *file = NULL;
    CHECK(fopen_s(&file, SHORT_FILE, "wb") == 0);
    CHECK(fwrite("hello!!", 1u, 7u, file) == 7u);
    CHECK(fclose(file) == 0);
    CHECK(tsdb_open_readonly(SHORT_FILE, &db) == TSDB_ERR_FORMAT);
    CHECK(db == NULL && file_length(SHORT_FILE) == 7);
    CHECK(tsdb_repair(SHORT_FILE, BACKUP_FILE, NULL) == TSDB_ERR_FORMAT);
    CHECK(file_length(SHORT_FILE) == 7);

    CHECK(fopen_s(&file, V1_FILE, "wb") == 0);
    CHECK(write_legacy_batch(file, 1u, 1));
    CHECK(write_legacy_batch(file, 2u, 2));
    CHECK(fclose(file) == 0);
    CHECK(tsdb_open_readonly(V1_FILE, &db) == TSDB_OK);
    CHECK(tsdb_write_batch(db, &point, 1u) == TSDB_ERR_READ_ONLY);
    TsdbPoint *results = NULL;
    size_t count = 0u;
    CHECK(tsdb_query_range(db, "cpu", 0, 10, &results, &count) == TSDB_OK);
    CHECK(count == 2u && results[0].value == 1.0 && results[1].value == 2.0);
    tsdb_free_points(results);
    CHECK(tsdb_close(db) == TSDB_OK);
    db = NULL;

    CHECK(tsdb_open(V1_FILE, &db) == TSDB_OK);
    point.timestamp_ms = 3;
    point.value = 3.0;
    CHECK(tsdb_write_batch(db, &point, 1u) == TSDB_OK);
    CHECK(tsdb_close(db) == TSDB_OK);
    db = NULL;
    long v1_length = 3L * (TSDB_WAL_BEGIN_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE);
    CHECK(file_length(V1_FILE) == v1_length);
    const long legacy_batch_size =
        (long)(TSDB_WAL_BEGIN_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE);
    CHECK(change_byte(V1_FILE, legacy_batch_size + 4L, 2));
    CHECK(tsdb_open_readonly(V1_FILE, &db) == TSDB_ERR_INCOMPLETE);
    CHECK(db == NULL && file_length(V1_FILE) == v1_length);
    CHECK(tsdb_repair(V1_FILE, BACKUP_FILE, NULL) == TSDB_ERR_FORMAT);
    CHECK(file_length(V1_FILE) == v1_length && file_length(BACKUP_FILE) == -1);
    CHECK(change_byte(V1_FILE, legacy_batch_size + 4L, 2));
    CHECK(change_byte(V1_FILE, 4L, 5));
    CHECK(tsdb_open_readonly(V1_FILE, &db) == TSDB_ERR_INCOMPLETE);
    CHECK(db == NULL && file_length(V1_FILE) == v1_length);
    CHECK(tsdb_repair(V1_FILE, BACKUP_FILE, NULL) == TSDB_ERR_FORMAT);
    CHECK(file_length(V1_FILE) == v1_length);

    CHECK(remove(V2_FILE) == 0);
    CHECK(remove(V1_FILE) == 0);
    CHECK(remove(SHORT_FILE) == 0);
    puts("WAL safety tests passed.");
    return 0;
}
