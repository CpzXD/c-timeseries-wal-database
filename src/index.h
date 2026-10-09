#ifndef TSDB_INTERNAL_INDEX_H
#define TSDB_INTERNAL_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "tsdb/record.h"

typedef struct {
    TsdbPoint *items;
    size_t count;
    size_t capacity;
} TsdbIndex;

void tsdb_index_destroy(TsdbIndex *index);
TsdbStatus tsdb_index_reserve(TsdbIndex *index, size_t additional);
void tsdb_index_insert_reserved(TsdbIndex *index, const TsdbPoint *point);
size_t tsdb_index_lower_bound(const TsdbIndex *index, const char *metric, int64_t timestamp_ms);
size_t tsdb_index_upper_bound(const TsdbIndex *index, const char *metric, int64_t timestamp_ms);

#endif
