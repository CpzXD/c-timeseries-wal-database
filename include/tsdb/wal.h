#ifndef TSDB_WAL_H
#define TSDB_WAL_H

#include <stddef.h>
#include <stdint.h>

#include "tsdb/record.h"

#define TSDB_WAL_MAX_BATCH_POINTS 4096u

typedef struct TsdbWal TsdbWal;

typedef enum { TSDB_WAL_READ_ONLY = 0, TSDB_WAL_READ_WRITE = 1 } TsdbWalMode;

typedef TsdbStatus (*TsdbWalBatchVisitor)(const TsdbPoint *points, size_t count,
                                          uint64_t sequence, void *context);

TsdbStatus tsdb_wal_create(const char *path, TsdbWal **out);

TsdbStatus tsdb_wal_open(const char *path, TsdbWalMode mode, TsdbWal **out);

TsdbStatus tsdb_wal_repair(const char *path, const char *backup_path,
                           uint64_t *out_discarded_bytes);

TsdbStatus tsdb_wal_write_batch(TsdbWal *wal, const TsdbPoint *points, size_t count,
                                uint64_t *out_sequence);

TsdbStatus tsdb_wal_replay(TsdbWal *wal, TsdbWalBatchVisitor visitor, void *context,
                           size_t *out_batch_count);

TsdbStatus tsdb_wal_close(TsdbWal *wal);

#endif
