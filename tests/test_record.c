#include "tsdb/record.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                            \
    do {                                                                                       \
        if (!(expr)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                    \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

static int test_round_trip(void)
{
    TsdbPoint original = {{0}, -1234567890123LL, -0.0};
    TsdbPoint decoded = {{0}, 0, 0.0};
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    uint64_t original_bits = 0;
    uint64_t decoded_bits = 0;

    memcpy(original.metric, "system.cpu.percent", sizeof "system.cpu.percent");
    CHECK(tsdb_record_encode(&original, raw) == TSDB_OK);
    CHECK(tsdb_record_decode(raw, &decoded) == TSDB_OK);
    CHECK(strcmp(decoded.metric, original.metric) == 0);
    CHECK(decoded.timestamp_ms == original.timestamp_ms);
    memcpy(&original_bits, &original.value, sizeof original_bits);
    memcpy(&decoded_bits, &decoded.value, sizeof decoded_bits);
    CHECK(decoded_bits == original_bits);
    return 0;
}

static int test_disk_layout(void)
{
    TsdbPoint point = {{0}, 0x0102030405060708LL, 1.0};
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    const uint8_t expected_time[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    const uint8_t expected_value[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F};

    memcpy(point.metric, "cpu", sizeof "cpu");
    CHECK(tsdb_record_encode(&point, raw) == TSDB_OK);
    CHECK(memcmp(raw, "TSR1", 4) == 0);
    CHECK(raw[4] == 1);
    CHECK(raw[5] == 3);
    CHECK(raw[6] == 0 && raw[7] == 0);
    CHECK(memcmp(raw + 8, expected_time, 8) == 0);
    CHECK(memcmp(raw + 16, expected_value, 8) == 0);
    CHECK(memcmp(raw + 24, "cpu", 3) == 0);
    CHECK(raw[27] == 0 && raw[87] == 0);

    CHECK(raw[88] == 0x92 && raw[89] == 0xEF);
    CHECK(raw[90] == 0x69 && raw[91] == 0x7A);
    return 0;
}

static int test_corruption(void)
{
    TsdbPoint point = {{0}, 42, 12.5};
    TsdbPoint decoded = {{0}, 0, 99.0};
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    memcpy(point.metric, "ram.bytes", sizeof "ram.bytes");

    CHECK(tsdb_record_encode(&point, raw) == TSDB_OK);
    raw[16] ^= 0x01;
    CHECK(tsdb_record_decode(raw, &decoded) == TSDB_ERR_CHECKSUM);
    CHECK(decoded.value == 99.0);
    raw[16] ^= 0x01;
    raw[0] = 'X';
    CHECK(tsdb_record_decode(raw, &decoded) == TSDB_ERR_FORMAT);
    return 0;
}

static int test_invalid_input(void)
{
    TsdbPoint point = {{0}, 0, 1.0};
    uint8_t raw[TSDB_RECORD_SIZE] = {0};

    CHECK(tsdb_record_encode(NULL, raw) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_record_encode(&point, NULL) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_record_decode(NULL, &point) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_record_decode(raw, NULL) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_record_encode(&point, raw) == TSDB_ERR_ARGUMENT);
    memcpy(point.metric, "cpu percent", sizeof "cpu percent");
    CHECK(tsdb_record_encode(&point, raw) == TSDB_ERR_ARGUMENT);
    memcpy(point.metric, "cpu", sizeof "cpu");
    point.value = INFINITY;
    CHECK(tsdb_record_encode(&point, raw) == TSDB_ERR_ARGUMENT);
    return 0;
}

static int test_metric_limits(void)
{
    TsdbPoint point = {{0}, INT64_MAX, 1.0};
    memset(point.metric, 'a', TSDB_METRIC_CAPACITY - 1u);
    uint8_t raw[TSDB_RECORD_SIZE] = {0};
    CHECK(tsdb_record_encode(&point, raw) == TSDB_OK);

    TsdbPoint decoded = {0};
    CHECK(tsdb_record_decode(raw, &decoded) == TSDB_OK);
    CHECK(strcmp(decoded.metric, point.metric) == 0);
    CHECK(decoded.timestamp_ms == INT64_MAX);

    uint8_t unchanged[TSDB_RECORD_SIZE] = {0};
    memcpy(unchanged, raw, sizeof raw);
    memset(point.metric, 'a', sizeof point.metric);
    CHECK(tsdb_record_encode(&point, raw) == TSDB_ERR_ARGUMENT);
    CHECK(memcmp(raw, unchanged, sizeof raw) == 0);
    memcpy(point.metric, "cpu", sizeof "cpu");
    point.value = NAN;
    CHECK(tsdb_record_encode(&point, raw) == TSDB_ERR_ARGUMENT);
    CHECK(memcmp(raw, unchanged, sizeof raw) == 0);
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= test_round_trip();
    failed |= test_disk_layout();
    failed |= test_corruption();
    failed |= test_invalid_input();
    failed |= test_metric_limits();
    if (failed == 0) {
        puts("All Lesson 1 record tests passed.");
    }
    return failed;
}
