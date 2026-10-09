#include <stdio.h>
#include <string.h>

#include "tsdb/crc32.h"
#include "tsdb/record.h"
#include "tsdb/wal_format.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

static TsdbPoint sample_point(void)
{
    TsdbPoint point = {{0}, 1234, 42.5};
    memcpy(point.metric, "cpu", sizeof "cpu");
    return point;
}

static uint32_t checksum(const uint8_t *bytes, size_t length)
{
    uint32_t state = tsdb_crc32_start();
    state = tsdb_crc32_update(state, bytes, length);
    return tsdb_crc32_finish(state);
}

static int write_file(const char *path, const uint8_t *bytes, size_t length)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "wb") != 0) {
        return 0;
    }
    size_t written = fwrite(bytes, 1u, length, file);
    int closed = fclose(file) == 0;
    return written == length && closed;
}

static int read_exact_file(const char *path, uint8_t *bytes, size_t length)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb") != 0) {
        return 0;
    }
    size_t read_count = fread(bytes, 1u, length, file);
    uint8_t extra_byte = 0u;
    size_t extra_count = fread(&extra_byte, 1u, 1u, file);
    int valid = read_count == length && extra_count == 0u && feof(file) && !ferror(file);
    return fclose(file) == 0 && valid;
}

static int test_record_file_round_trip(void)
{
    TsdbPoint original = sample_point();
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    uint8_t loaded[TSDB_RECORD_SIZE] = {0};
    CHECK(tsdb_record_encode(&original, raw) == TSDB_OK);
    CHECK(write_file("wal_smoke.bin", raw, sizeof raw));
    CHECK(read_exact_file("wal_smoke.bin", loaded, sizeof loaded));
    CHECK(memcmp(loaded, raw, sizeof raw) == 0);

    TsdbPoint recovered = {0};
    CHECK(tsdb_record_decode(loaded, &recovered) == TSDB_OK);
    CHECK(strcmp(recovered.metric, original.metric) == 0);
    CHECK(recovered.timestamp_ms == original.timestamp_ms);
    CHECK(recovered.value == original.value);
    CHECK(remove("wal_smoke.bin") == 0);
    return 0;
}

static int test_incomplete_record_file(void)
{
    TsdbPoint point = sample_point();
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    uint8_t file_bytes[TSDB_RECORD_SIZE + 10u] = {0};
    CHECK(tsdb_record_encode(&point, raw) == TSDB_OK);
    memcpy(file_bytes, raw, sizeof raw);
    memcpy(file_bytes + sizeof raw, raw, 10u);
    CHECK(write_file("wal_truncated.bin", file_bytes, sizeof file_bytes));

    FILE *file = NULL;
    CHECK(fopen_s(&file, "wal_truncated.bin", "rb") == 0);
    uint8_t loaded[TSDB_RECORD_SIZE] = {0};
    CHECK(fread(loaded, 1u, sizeof loaded, file) == sizeof loaded);
    CHECK(memcmp(loaded, raw, sizeof raw) == 0);
    CHECK(fread(loaded, 1u, sizeof loaded, file) == 10u);
    CHECK(feof(file) && !ferror(file));
    CHECK(fclose(file) == 0);
    CHECK(remove("wal_truncated.bin") == 0);
    return 0;
}

static int test_begin_v1_format(void)
{
    const uint8_t expected[TSDB_WAL_BEGIN_SIZE] = {'T', 'B', 'G', '1', 3, 0, 0, 0,
                                                   9,   0,   0,   0,   0, 0, 0, 0};
    uint8_t begin[TSDB_WAL_BEGIN_SIZE] = {0};
    CHECK(tsdb_wal_begin_encode(3u, 9u, begin) == TSDB_OK);
    CHECK(memcmp(begin, expected, sizeof begin) == 0);
    CHECK(tsdb_wal_begin_encode(0u, 9u, begin) == TSDB_ERR_ARGUMENT);
    CHECK(memcmp(begin, expected, sizeof begin) == 0);
    CHECK(tsdb_wal_begin_encode(3u, 9u, NULL) == TSDB_ERR_ARGUMENT);

    uint32_t count = 0u;
    uint64_t sequence = 0u;
    begin[0] = 'X';
    CHECK(tsdb_wal_begin_decode(begin, &count, &sequence) == TSDB_ERR_FORMAT);
    memcpy(begin, expected, sizeof begin);
    begin[4] = 0u;
    CHECK(tsdb_wal_begin_decode(begin, &count, &sequence) == TSDB_ERR_FORMAT);
    CHECK(tsdb_wal_begin_decode(expected, &count, &sequence) == TSDB_OK);
    CHECK(count == 3u && sequence == 9u);

    const uint8_t multiple_bytes[TSDB_WAL_BEGIN_SIZE] = {'T', 'B', 'G', '1', 2, 1, 0, 0,
                                                         1,   2,   0,   0,   0, 0, 0, 0};
    CHECK(tsdb_wal_begin_decode(multiple_bytes, &count, &sequence) == TSDB_OK);
    CHECK(count == 258u && sequence == 513u);
    return 0;
}

static int test_commit_format(void)
{
    const uint8_t expected[TSDB_WAL_COMMIT_SIZE] = {'T', 'C', 'M', '1', 9,    0,    0,    0,
                                                    0,   0,   0,   0,   0x78, 0x56, 0x34, 0x12};
    uint8_t commit[TSDB_WAL_COMMIT_SIZE] = {0};
    CHECK(tsdb_wal_commit_encode(9u, 0x12345678u, commit) == TSDB_OK);
    CHECK(memcmp(commit, expected, sizeof commit) == 0);

    uint64_t sequence = 0u;
    uint32_t crc = 0u;
    CHECK(tsdb_wal_commit_decode(expected, &sequence, &crc) == TSDB_OK);
    CHECK(sequence == 9u && crc == 0x12345678u);
    return 0;
}

static int test_incremental_crc(void)
{
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t state = tsdb_crc32_start();
    state = tsdb_crc32_update(state, digits, 4u);
    state = tsdb_crc32_update(state, digits + 4u, 5u);
    CHECK(tsdb_crc32_finish(state) == 0xCBF43926u);
    return 0;
}

static int test_legacy_batch_file_round_trip(void)
{
    TsdbPoint original = sample_point();
    uint8_t batch[TSDB_WAL_BEGIN_SIZE + TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE] = {0};
    const size_t commit_offset = TSDB_WAL_BEGIN_SIZE + TSDB_RECORD_SIZE;
    CHECK(tsdb_wal_begin_encode(1u, 9u, batch) == TSDB_OK);
    CHECK(tsdb_record_encode(&original, batch + TSDB_WAL_BEGIN_SIZE) == TSDB_OK);
    uint32_t batch_crc = checksum(batch, commit_offset);
    CHECK(batch_crc == 0x4368200Du);
    CHECK(tsdb_wal_commit_encode(9u, batch_crc, batch + commit_offset) == TSDB_OK);
    CHECK(write_file("wal_batch.bin", batch, sizeof batch));

    uint8_t loaded[sizeof batch] = {0};
    CHECK(read_exact_file("wal_batch.bin", loaded, sizeof loaded));
    CHECK(memcmp(loaded, batch, sizeof loaded) == 0);
    uint32_t count = 0u;
    uint64_t sequence = 0u;
    CHECK(tsdb_wal_begin_decode(loaded, &count, &sequence) == TSDB_OK);
    CHECK(count == 1u && sequence == 9u);

    TsdbPoint recovered = {0};
    CHECK(tsdb_record_decode(loaded + TSDB_WAL_BEGIN_SIZE, &recovered) == TSDB_OK);
    CHECK(strcmp(recovered.metric, original.metric) == 0);
    CHECK(recovered.timestamp_ms == original.timestamp_ms);
    CHECK(recovered.value == original.value);

    uint64_t commit_sequence = 0u;
    uint32_t stored_crc = 0u;
    CHECK(tsdb_wal_commit_decode(loaded + commit_offset, &commit_sequence, &stored_crc) ==
          TSDB_OK);
    CHECK(commit_sequence == sequence);
    CHECK(checksum(loaded, commit_offset) == stored_crc);
    loaded[TSDB_WAL_BEGIN_SIZE + 16u] ^= 1u;
    CHECK(checksum(loaded, commit_offset) != stored_crc);
    CHECK(remove("wal_batch.bin") == 0);
    return 0;
}

int main(void)
{
    CHECK(test_record_file_round_trip() == 0);
    CHECK(test_incomplete_record_file() == 0);
    CHECK(test_begin_v1_format() == 0);
    CHECK(test_commit_format() == 0);
    CHECK(test_incremental_crc() == 0);
    CHECK(test_legacy_batch_file_round_trip() == 0);
    puts("Recordul a fost scris si citit corect.");
    return 0;
}
