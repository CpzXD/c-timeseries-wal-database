#include "index.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int compare_key(const TsdbPoint *point, const char *metric, int64_t timestamp_ms)
{
    int name_order = strcmp(point->metric, metric);
    if (name_order != 0) {
        return name_order;
    }
    if (point->timestamp_ms < timestamp_ms) {
        return -1;
    }
    if (point->timestamp_ms > timestamp_ms) {
        return 1;
    }
    return 0;
}

void tsdb_index_destroy(TsdbIndex *index)
{
    free(index->items);
    index->items = NULL;
    index->count = 0u;
    index->capacity = 0u;
}

TsdbStatus tsdb_index_reserve(TsdbIndex *index, size_t additional)
{
    if (additional > SIZE_MAX - index->count) {
        return TSDB_ERR_LIMIT;
    }
    size_t needed = index->count + additional;
    if (needed <= index->capacity) {
        return TSDB_OK;
    }
    if (needed > SIZE_MAX / sizeof *index->items) {
        return TSDB_ERR_LIMIT;
    }

    size_t max_capacity = SIZE_MAX / sizeof *index->items;
    size_t capacity = index->capacity == 0u ? 16u : index->capacity;
    while (capacity < needed) {
        if (capacity > max_capacity / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > max_capacity) {
        capacity = needed;
    }
    TsdbPoint *new_items = (TsdbPoint *)realloc(index->items, capacity * sizeof *index->items);
    if (new_items == NULL) {
        return TSDB_ERR_MEMORY;
    }
    index->items = new_items;
    index->capacity = capacity;
    return TSDB_OK;
}

size_t tsdb_index_lower_bound(const TsdbIndex *index, const char *metric, int64_t timestamp_ms)
{
    size_t lo = 0u;
    size_t hi = index->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (compare_key(&index->items[mid], metric, timestamp_ms) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t tsdb_index_upper_bound(const TsdbIndex *index, const char *metric, int64_t timestamp_ms)
{
    size_t lo = 0u;
    size_t hi = index->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (compare_key(&index->items[mid], metric, timestamp_ms) <= 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void tsdb_index_insert_reserved(TsdbIndex *index, const TsdbPoint *point)
{
    size_t position = tsdb_index_lower_bound(index, point->metric, point->timestamp_ms);
    if (position < index->count &&
        compare_key(&index->items[position], point->metric, point->timestamp_ms) == 0) {
        index->items[position] = *point;
        return;
    }

    if (position < index->count) {
        memmove(&index->items[position + 1u], &index->items[position],
                (index->count - position) * sizeof *index->items);
    }
    index->items[position] = *point;
    ++index->count;
}
