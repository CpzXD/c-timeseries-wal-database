#include "tsdb/wal_format.h"

#include <stddef.h>
#include <string.h>

#include "binary.h"
#include "tsdb/crc32.h"

enum {
    FILE_HEADER_CRC_OFFSET = 12,
    BEGIN_COUNT_OFFSET = 4,
    BEGIN_SEQUENCE_OFFSET = 8,
    BEGIN_V2_CRC_OFFSET = 16,
    COMMIT_SEQUENCE_OFFSET = 4,
    COMMIT_CRC_OFFSET = 12
};

static uint32_t prefix_crc(const uint8_t *bytes, size_t length)
{
    uint32_t state = tsdb_crc32_start();
    state = tsdb_crc32_update(state, bytes, length);
    return tsdb_crc32_finish(state);
}

TsdbStatus tsdb_wal_file_header_encode(uint8_t out[TSDB_WAL_FILE_HEADER_SIZE])
{
    if (out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }

    uint8_t header[TSDB_WAL_FILE_HEADER_SIZE] = {0};
    memcpy(header, "TSDBWAL2", 8u);
    tsdb_put_u32_le(header + FILE_HEADER_CRC_OFFSET,
                    prefix_crc(header, FILE_HEADER_CRC_OFFSET));
    memcpy(out, header, sizeof header);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_file_header_decode(const uint8_t in[TSDB_WAL_FILE_HEADER_SIZE])
{
    if (in == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (memcmp(in, "TSDBWAL2", 8u) != 0 || in[8] != 0u || in[9] != 0u || in[10] != 0u ||
        in[11] != 0u) {
        return TSDB_ERR_FORMAT;
    }
    if (tsdb_get_u32_le(in + FILE_HEADER_CRC_OFFSET) !=
        prefix_crc(in, FILE_HEADER_CRC_OFFSET)) {
        return TSDB_ERR_CHECKSUM;
    }
    return TSDB_OK;
}

TsdbStatus tsdb_wal_begin_v2_encode(uint32_t count, uint64_t sequence,
                                    uint8_t out[TSDB_WAL_BEGIN_V2_SIZE])
{
    if (out == NULL || count == 0u) {
        return TSDB_ERR_ARGUMENT;
    }

    uint8_t begin[TSDB_WAL_BEGIN_V2_SIZE] = {0};
    memcpy(begin, "TBG2", 4u);
    tsdb_put_u32_le(begin + BEGIN_COUNT_OFFSET, count);
    tsdb_put_u64_le(begin + BEGIN_SEQUENCE_OFFSET, sequence);
    tsdb_put_u32_le(begin + BEGIN_V2_CRC_OFFSET, prefix_crc(begin, BEGIN_V2_CRC_OFFSET));
    memcpy(out, begin, sizeof begin);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_begin_v2_decode(const uint8_t in[TSDB_WAL_BEGIN_V2_SIZE], uint32_t *count,
                                    uint64_t *sequence)
{
    if (in == NULL || count == NULL || sequence == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (memcmp(in, "TBG2", 4u) != 0) {
        return TSDB_ERR_FORMAT;
    }
    if (tsdb_get_u32_le(in + BEGIN_V2_CRC_OFFSET) != prefix_crc(in, BEGIN_V2_CRC_OFFSET)) {
        return TSDB_ERR_CHECKSUM;
    }

    uint32_t decoded_count = tsdb_get_u32_le(in + BEGIN_COUNT_OFFSET);
    if (decoded_count == 0u) {
        return TSDB_ERR_FORMAT;
    }

    *count = decoded_count;
    *sequence = tsdb_get_u64_le(in + BEGIN_SEQUENCE_OFFSET);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_begin_encode(uint32_t count, uint64_t sequence,
                                 uint8_t out[TSDB_WAL_BEGIN_SIZE])
{
    if (out == NULL || count == 0u) {
        return TSDB_ERR_ARGUMENT;
    }

    uint8_t begin[TSDB_WAL_BEGIN_SIZE] = {0};
    memcpy(begin, "TBG1", 4u);
    tsdb_put_u32_le(begin + BEGIN_COUNT_OFFSET, count);
    tsdb_put_u64_le(begin + BEGIN_SEQUENCE_OFFSET, sequence);
    memcpy(out, begin, sizeof begin);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_begin_decode(const uint8_t in[TSDB_WAL_BEGIN_SIZE], uint32_t *count,
                                 uint64_t *sequence)
{
    if (in == NULL || count == NULL || sequence == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (memcmp(in, "TBG1", 4u) != 0) {
        return TSDB_ERR_FORMAT;
    }

    uint32_t decoded_count = tsdb_get_u32_le(in + BEGIN_COUNT_OFFSET);
    if (decoded_count == 0u) {
        return TSDB_ERR_FORMAT;
    }

    *count = decoded_count;
    *sequence = tsdb_get_u64_le(in + BEGIN_SEQUENCE_OFFSET);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_commit_encode(uint64_t sequence, uint32_t batch_crc,
                                  uint8_t out[TSDB_WAL_COMMIT_SIZE])
{
    if (out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }

    uint8_t commit[TSDB_WAL_COMMIT_SIZE] = {0};
    memcpy(commit, "TCM1", 4u);
    tsdb_put_u64_le(commit + COMMIT_SEQUENCE_OFFSET, sequence);
    tsdb_put_u32_le(commit + COMMIT_CRC_OFFSET, batch_crc);
    memcpy(out, commit, sizeof commit);
    return TSDB_OK;
}

TsdbStatus tsdb_wal_commit_decode(const uint8_t in[TSDB_WAL_COMMIT_SIZE], uint64_t *sequence,
                                  uint32_t *batch_crc)
{
    if (in == NULL || sequence == NULL || batch_crc == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (memcmp(in, "TCM1", 4u) != 0) {
        return TSDB_ERR_FORMAT;
    }

    *sequence = tsdb_get_u64_le(in + COMMIT_SEQUENCE_OFFSET);
    *batch_crc = tsdb_get_u32_le(in + COMMIT_CRC_OFFSET);
    return TSDB_OK;
}
