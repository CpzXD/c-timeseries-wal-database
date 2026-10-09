#ifndef TSDB_WAL_FORMAT_H
#define TSDB_WAL_FORMAT_H

#include <stdint.h>
#include "tsdb/record.h"

#define TSDB_WAL_BEGIN_SIZE 16u
#define TSDB_WAL_BEGIN_V2_SIZE 20u
#define TSDB_WAL_FILE_HEADER_SIZE 16u
#define TSDB_WAL_COMMIT_SIZE 16u

TsdbStatus tsdb_wal_file_header_encode(uint8_t out[TSDB_WAL_FILE_HEADER_SIZE]);
TsdbStatus tsdb_wal_file_header_decode(const uint8_t in[TSDB_WAL_FILE_HEADER_SIZE]);

TsdbStatus tsdb_wal_begin_v2_encode(uint32_t count, uint64_t sequence,
                                    uint8_t out[TSDB_WAL_BEGIN_V2_SIZE]);
TsdbStatus tsdb_wal_begin_v2_decode(const uint8_t in[TSDB_WAL_BEGIN_V2_SIZE], uint32_t *count,
                                    uint64_t *sequence);

TsdbStatus tsdb_wal_begin_encode(uint32_t count, uint64_t sequence,
                                 uint8_t out[TSDB_WAL_BEGIN_SIZE]);

TsdbStatus tsdb_wal_begin_decode(const uint8_t in[TSDB_WAL_BEGIN_SIZE], uint32_t *count,
                                 uint64_t *sequence);

TsdbStatus tsdb_wal_commit_encode(uint64_t sequence, uint32_t batch_crc,
                                  uint8_t out[TSDB_WAL_COMMIT_SIZE]);

TsdbStatus tsdb_wal_commit_decode(const uint8_t in[TSDB_WAL_COMMIT_SIZE], uint64_t *sequence,
                                  uint32_t *batch_crc);

#endif
