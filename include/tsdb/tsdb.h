#ifndef TSDB_TSDB_H
#define TSDB_TSDB_H

#include <stddef.h>
#include <stdint.h>

#include "tsdb/record.h"

typedef struct Tsdb Tsdb;

typedef struct {
    size_t count;
    double min;
    double max;
    double avg;
} TsdbAggregate;

TsdbStatus tsdb_create(const char *path, Tsdb **out);

TsdbStatus tsdb_open(const char *path, Tsdb **out);

TsdbStatus tsdb_open_readonly(const char *path, Tsdb **out);

TsdbStatus tsdb_repair(const char *path, const char *backup_path,
                       uint64_t *out_discarded_bytes);

TsdbStatus tsdb_write_batch(Tsdb *db, const TsdbPoint *points, size_t count);

TsdbStatus tsdb_query_range(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms,
                            TsdbPoint **out_points, size_t *out_count);

TsdbStatus tsdb_aggregate(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms,
                          TsdbAggregate *out);

void tsdb_free_points(TsdbPoint *points);
TsdbStatus tsdb_close(Tsdb *db);

#endif
