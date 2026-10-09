#include "tsdb/record.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "binary.h"
#include "tsdb/crc32.h"

enum {
    RECORD_VERSION_OFFSET = 4,
    RECORD_NAME_LENGTH_OFFSET = 5,
    RECORD_RESERVED_OFFSET = 6,
    RECORD_TIMESTAMP_OFFSET = 8,
    RECORD_VALUE_OFFSET = 16,
    RECORD_METRIC_OFFSET = 24,
    RECORD_CRC_OFFSET = 88
};

_Static_assert(sizeof(double) == sizeof(uint64_t), "Records require 64-bit doubles.");
_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "Records require binary64 doubles.");

static int metric_character_allowed(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' ||
           character == '.';
}

static size_t valid_metric_length(const char *metric)
{
    for (size_t length = 0u; length < TSDB_METRIC_CAPACITY; ++length) {
        if (metric[length] == '\0') {
            return length;
        }
        if (!metric_character_allowed(metric[length])) {
            return 0u;
        }
    }
    return 0u;
}

static uint32_t record_crc(const uint8_t *record)
{
    uint32_t state = tsdb_crc32_start();
    state = tsdb_crc32_update(state, record, RECORD_CRC_OFFSET);
    return tsdb_crc32_finish(state);
}

TsdbStatus tsdb_record_encode(const TsdbPoint *point, uint8_t out[TSDB_RECORD_SIZE])
{
    if (point == NULL || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }

    size_t metric_length = valid_metric_length(point->metric);
    if (metric_length == 0u || !isfinite(point->value)) {
        return TSDB_ERR_ARGUMENT;
    }

    uint8_t record[TSDB_RECORD_SIZE] = {0};
    memcpy(record, "TSR1", 4u);
    record[RECORD_VERSION_OFFSET] = 1u;
    record[RECORD_NAME_LENGTH_OFFSET] = (uint8_t)metric_length;
    memcpy(record + RECORD_METRIC_OFFSET, point->metric, metric_length);

    uint64_t timestamp_bits = 0u;
    uint64_t value_bits = 0u;
    memcpy(&timestamp_bits, &point->timestamp_ms, sizeof timestamp_bits);
    memcpy(&value_bits, &point->value, sizeof value_bits);
    tsdb_put_u64_le(record + RECORD_TIMESTAMP_OFFSET, timestamp_bits);
    tsdb_put_u64_le(record + RECORD_VALUE_OFFSET, value_bits);
    tsdb_put_u32_le(record + RECORD_CRC_OFFSET, record_crc(record));

    memcpy(out, record, sizeof record);
    return TSDB_OK;
}

TsdbStatus tsdb_record_decode(const uint8_t in[TSDB_RECORD_SIZE], TsdbPoint *out)
{
    if (in == NULL || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (memcmp(in, "TSR1", 4u) != 0 || in[RECORD_VERSION_OFFSET] != 1u) {
        return TSDB_ERR_FORMAT;
    }

    size_t metric_length = in[RECORD_NAME_LENGTH_OFFSET];
    if (metric_length == 0u || metric_length >= TSDB_METRIC_CAPACITY ||
        in[RECORD_RESERVED_OFFSET] != 0u || in[RECORD_RESERVED_OFFSET + 1u] != 0u) {
        return TSDB_ERR_FORMAT;
    }
    if (tsdb_get_u32_le(in + RECORD_CRC_OFFSET) != record_crc(in)) {
        return TSDB_ERR_CHECKSUM;
    }

    TsdbPoint decoded = {0};
    uint64_t timestamp_bits = tsdb_get_u64_le(in + RECORD_TIMESTAMP_OFFSET);
    uint64_t value_bits = tsdb_get_u64_le(in + RECORD_VALUE_OFFSET);
    memcpy(&decoded.timestamp_ms, &timestamp_bits, sizeof decoded.timestamp_ms);
    memcpy(&decoded.value, &value_bits, sizeof decoded.value);
    memcpy(decoded.metric, in + RECORD_METRIC_OFFSET, metric_length);

    if (valid_metric_length(decoded.metric) != metric_length || !isfinite(decoded.value)) {
        return TSDB_ERR_FORMAT;
    }
    for (size_t byte = RECORD_METRIC_OFFSET + metric_length; byte < RECORD_CRC_OFFSET; ++byte) {
        if (in[byte] != 0u) {
            return TSDB_ERR_FORMAT;
        }
    }

    *out = decoded;
    return TSDB_OK;
}
