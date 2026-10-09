#ifndef TSDB_WAL_FAULT_HOOKS_H
#define TSDB_WAL_FAULT_HOOKS_H

#include <stddef.h>

#include "tsdb/wal.h"

typedef void (*TsdbWalTestCheckpoint)(void *context);

void tsdb_wal_test_fail_write(TsdbWal *wal, size_t bytes_before_failure, int fail_rollback);
void tsdb_wal_test_fail_flush(TsdbWal *wal, int fail_rollback);
void tsdb_wal_test_pause_write(TsdbWal *wal, size_t bytes_before_pause,
                               TsdbWalTestCheckpoint checkpoint, void *context);

#endif
