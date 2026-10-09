#ifndef TSDB_RECORD_H
#define TSDB_RECORD_H

#include <stdint.h>

#define TSDB_METRIC_CAPACITY 64u
#define TSDB_RECORD_SIZE 92u

typedef struct {
    char metric[TSDB_METRIC_CAPACITY];
    int64_t timestamp_ms;
    double value;
} TsdbPoint;

typedef enum {
    TSDB_OK = 0,
    TSDB_ERR_ARGUMENT,
    TSDB_ERR_FORMAT,
    TSDB_ERR_CHECKSUM,
    TSDB_ERR_NOT_IMPLEMENTED,
    TSDB_ERR_IO,
    TSDB_ERR_MEMORY,
    TSDB_ERR_LIMIT,
    TSDB_ERR_INCOMPLETE,
    TSDB_ERR_READ_ONLY,
    TSDB_ERR_EXISTS,
    TSDB_ERR_NOT_FOUND
} TsdbStatus;

TsdbStatus tsdb_record_encode(const TsdbPoint *point, uint8_t out[TSDB_RECORD_SIZE]);

TsdbStatus tsdb_record_decode(const uint8_t in[TSDB_RECORD_SIZE], TsdbPoint *out);

#endif
