#include "tsdb/tsdb.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "index.h"
#include "tsdb/wal.h"

struct Tsdb {
    TsdbWal *wal;
    TsdbIndex index;
    int writable;
    int failed;
};

static int valid_metric(const char *metric)
{
    if (metric == NULL) {
        return 0;
    }
    for (size_t i = 0u; i < TSDB_METRIC_CAPACITY; ++i) {
        char c = metric[i];
        if (c == '\0') {
            return i != 0u;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) {
            return 0;
        }
    }
    return 0;
}

static TsdbStatus replay_batch(const TsdbPoint *points, size_t count, uint64_t sequence,
                               void *context)
{
    Tsdb *db = (Tsdb *)context;
    (void)sequence;
    TsdbStatus status = tsdb_index_reserve(&db->index, count);
    if (status != TSDB_OK) {
        return status;
    }
    for (size_t i = 0u; i < count; ++i) {
        tsdb_index_insert_reserved(&db->index, &points[i]);
    }
    return TSDB_OK;
}

TsdbStatus tsdb_create(const char *path, Tsdb **out)
{
    if (path == NULL || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    *out = NULL;
    Tsdb *db = (Tsdb *)calloc(1u, sizeof *db);
    if (db == NULL) {
        return TSDB_ERR_MEMORY;
    }
    TsdbStatus status = tsdb_wal_create(path, &db->wal);
    if (status != TSDB_OK) {
        free(db);
        return status;
    }
    db->writable = 1;
    *out = db;
    return TSDB_OK;
}

static TsdbStatus open_existing(const char *path, TsdbWalMode mode, Tsdb **out)
{
    if (path == NULL || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    *out = NULL;
    Tsdb *db = (Tsdb *)calloc(1u, sizeof *db);
    if (db == NULL) {
        return TSDB_ERR_MEMORY;
    }
    TsdbStatus status = tsdb_wal_open(path, mode, &db->wal);
    if (status == TSDB_OK) {
        status = tsdb_wal_replay(db->wal, replay_batch, db, NULL);
    }
    if (status != TSDB_OK) {
        if (db->wal != NULL) {
            (void)tsdb_wal_close(db->wal);
        }
        tsdb_index_destroy(&db->index);
        free(db);
        return status;
    }
    db->writable = mode == TSDB_WAL_READ_WRITE;
    *out = db;
    return TSDB_OK;
}

TsdbStatus tsdb_open(const char *path, Tsdb **out)
{
    return open_existing(path, TSDB_WAL_READ_WRITE, out);
}

TsdbStatus tsdb_open_readonly(const char *path, Tsdb **out)
{
    return open_existing(path, TSDB_WAL_READ_ONLY, out);
}

TsdbStatus tsdb_repair(const char *path, const char *backup_path, uint64_t *out_discarded_bytes)
{
    return tsdb_wal_repair(path, backup_path, out_discarded_bytes);
}

TsdbStatus tsdb_write_batch(Tsdb *db, const TsdbPoint *points, size_t count)
{
    if (db == NULL || points == NULL || count == 0u) {
        return TSDB_ERR_ARGUMENT;
    }
    if (db->failed) {
        return TSDB_ERR_IO;
    }
    if (!db->writable) {
        return TSDB_ERR_READ_ONLY;
    }
    if (count > TSDB_WAL_MAX_BATCH_POINTS) {
        return TSDB_ERR_LIMIT;
    }

    TsdbStatus status = tsdb_index_reserve(&db->index, count);
    if (status != TSDB_OK) {
        return status;
    }
    status = tsdb_wal_write_batch(db->wal, points, count, NULL);
    if (status != TSDB_OK) {
        if (status == TSDB_ERR_IO) {
            db->failed = 1;
        }
        return status;
    }
    for (size_t i = 0u; i < count; ++i) {
        tsdb_index_insert_reserved(&db->index, &points[i]);
    }
    return TSDB_OK;
}

TsdbStatus tsdb_query_range(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms,
                            TsdbPoint **out_points, size_t *out_count)
{
    if (db == NULL || !valid_metric(metric) || from_ms > to_ms || out_points == NULL ||
        out_count == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (db->failed) {
        return TSDB_ERR_IO;
    }
    size_t start = tsdb_index_lower_bound(&db->index, metric, from_ms);
    size_t end = tsdb_index_upper_bound(&db->index, metric, to_ms);
    size_t count = end - start;
    TsdbPoint *result = NULL;
    if (count != 0u) {
        result = (TsdbPoint *)malloc(count * sizeof *result);
        if (result == NULL) {
            return TSDB_ERR_MEMORY;
        }
        memcpy(result, &db->index.items[start], count * sizeof *result);
    }
    *out_points = result;
    *out_count = count;
    return TSDB_OK;
}

TsdbStatus tsdb_aggregate(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms,
                          TsdbAggregate *out)
{
    if (db == NULL || !valid_metric(metric) || from_ms > to_ms || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (db->failed) {
        return TSDB_ERR_IO;
    }
    size_t start = tsdb_index_lower_bound(&db->index, metric, from_ms);
    size_t end = tsdb_index_upper_bound(&db->index, metric, to_ms);
    TsdbAggregate result = {0};
    for (size_t i = start; i < end; ++i) {
        double value = db->index.items[i].value;
        ++result.count;
        if (result.count == 1u) {
            result.min = value;
            result.max = value;
            result.avg = value;
        } else {
            if (value < result.min) {
                result.min = value;
            }
            if (value > result.max) {
                result.max = value;
            }

            double divisor = (double)result.count;
            result.avg += value / divisor - result.avg / divisor;
        }
    }
    *out = result;
    return TSDB_OK;
}

void tsdb_free_points(TsdbPoint *points)
{
    free(points);
}

TsdbStatus tsdb_close(Tsdb *db)
{
    if (db == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    TsdbStatus status = tsdb_wal_close(db->wal);
    tsdb_index_destroy(&db->index);
    free(db);
    return status;
}
