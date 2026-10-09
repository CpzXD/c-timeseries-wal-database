#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tsdb/tsdb.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

#define DB_FILE "query_model_test.bin"
#define METRIC_COUNT 3u
#define TIMESTAMP_COUNT 65u
#define BATCH_CAPACITY 17u

typedef struct {
    int present[METRIC_COUNT][TIMESTAMP_COUNT];
    double values[METRIC_COUNT][TIMESTAMP_COUNT];
} ReferenceModel;

static const char *const metrics[METRIC_COUNT] = {"cpu", "disk", "ram"};

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static int verify_range(Tsdb *db, const ReferenceModel *model, size_t metric_index,
                        int64_t from_ms, int64_t to_ms)
{
    TsdbPoint *results = NULL;
    size_t result_count = 0u;
    CHECK(tsdb_query_range(db, metrics[metric_index], from_ms, to_ms, &results,
                           &result_count) == TSDB_OK);

    size_t expected_count = 0u;
    double expected_min = 0.0;
    double expected_max = 0.0;
    double expected_sum = 0.0;
    for (size_t timestamp_index = 0u; timestamp_index < TIMESTAMP_COUNT; ++timestamp_index) {
        int64_t timestamp = (int64_t)timestamp_index - 32;
        if (!model->present[metric_index][timestamp_index] || timestamp < from_ms ||
            timestamp > to_ms) {
            continue;
        }

        CHECK(expected_count < result_count);
        double value = model->values[metric_index][timestamp_index];
        const TsdbPoint *point = &results[expected_count];
        CHECK(strcmp(point->metric, metrics[metric_index]) == 0);
        CHECK(point->timestamp_ms == timestamp && point->value == value);
        if (expected_count == 0u || value < expected_min) {
            expected_min = value;
        }
        if (expected_count == 0u || value > expected_max) {
            expected_max = value;
        }
        expected_sum += value;
        ++expected_count;
    }
    CHECK(result_count == expected_count);
    CHECK(expected_count != 0u || results == NULL);
    tsdb_free_points(results);

    double expected_average =
        expected_count == 0u ? 0.0 : expected_sum / (double)expected_count;
    TsdbAggregate aggregate = {0};
    CHECK(tsdb_aggregate(db, metrics[metric_index], from_ms, to_ms, &aggregate) == TSDB_OK);
    CHECK(aggregate.count == expected_count);
    CHECK(aggregate.min == expected_min && aggregate.max == expected_max);
    CHECK(fabs(aggregate.avg - expected_average) < 1e-12);
    return 0;
}

static int verify_model(Tsdb *db, const ReferenceModel *model)
{
    uint32_t random_state = 901u;
    for (size_t metric_index = 0u; metric_index < METRIC_COUNT; ++metric_index) {
        CHECK(verify_range(db, model, metric_index, INT64_MIN, INT64_MAX) == 0);
        for (size_t query = 0u; query < 100u; ++query) {
            int64_t first = (int64_t)(next_random(&random_state) % 81u) - 40;
            int64_t second = (int64_t)(next_random(&random_state) % 81u) - 40;
            int64_t from_ms = first < second ? first : second;
            int64_t to_ms = first < second ? second : first;
            CHECK(verify_range(db, model, metric_index, from_ms, to_ms) == 0);
            CHECK(verify_range(db, model, metric_index, first, first) == 0);
        }
    }
    return 0;
}

static int verify_extreme_timestamps(Tsdb *db)
{
    TsdbPoint *results = NULL;
    size_t count = 0u;
    CHECK(tsdb_query_range(db, "edge", INT64_MIN, INT64_MAX, &results, &count) == TSDB_OK);
    CHECK(count == 2u);
    CHECK(results[0].timestamp_ms == INT64_MIN && results[0].value == -2.0);
    CHECK(results[1].timestamp_ms == INT64_MAX && results[1].value == 4.0);
    tsdb_free_points(results);
    CHECK(tsdb_query_range(db, "edge", INT64_MAX, INT64_MAX, &results, &count) == TSDB_OK);
    CHECK(count == 1u && results[0].value == 4.0);
    tsdb_free_points(results);
    return 0;
}

int main(void)
{
    (void)remove(DB_FILE);
    Tsdb *db = NULL;
    CHECK(tsdb_create(DB_FILE, &db) == TSDB_OK);
    ReferenceModel model = {0};
    CHECK(verify_model(db, &model) == 0);

    uint32_t random_state = 77u;
    for (size_t batch_number = 0u; batch_number < 64u; ++batch_number) {
        TsdbPoint batch[BATCH_CAPACITY] = {0};
        size_t count = 1u + next_random(&random_state) % BATCH_CAPACITY;
        size_t metric_indices[BATCH_CAPACITY] = {0};
        size_t timestamp_indices[BATCH_CAPACITY] = {0};
        for (size_t point_index = 0u; point_index < count; ++point_index) {
            size_t metric_index = next_random(&random_state) % METRIC_COUNT;
            size_t timestamp_index = next_random(&random_state) % TIMESTAMP_COUNT;
            metric_indices[point_index] = metric_index;
            timestamp_indices[point_index] = timestamp_index;
            memcpy(batch[point_index].metric, metrics[metric_index],
                   strlen(metrics[metric_index]) + 1u);
            batch[point_index].timestamp_ms = (int64_t)timestamp_index - 32;
            batch[point_index].value =
                (double)((int)(next_random(&random_state) % 1025u) - 512) / 2.0;
        }
        CHECK(tsdb_write_batch(db, batch, count) == TSDB_OK);
        for (size_t point_index = 0u; point_index < count; ++point_index) {
            size_t metric_index = metric_indices[point_index];
            size_t timestamp_index = timestamp_indices[point_index];
            model.present[metric_index][timestamp_index] = 1;
            model.values[metric_index][timestamp_index] = batch[point_index].value;
        }
    }

    TsdbPoint edges[3] = {{{0}, INT64_MAX, 1.0}, {{0}, INT64_MIN, -2.0}, {{0}, INT64_MAX, 4.0}};
    for (size_t point_index = 0u; point_index < 3u; ++point_index) {
        memcpy(edges[point_index].metric, "edge", sizeof "edge");
    }
    CHECK(tsdb_write_batch(db, edges, 3u) == TSDB_OK);
    CHECK(verify_model(db, &model) == 0);
    CHECK(verify_extreme_timestamps(db) == 0);
    CHECK(tsdb_close(db) == TSDB_OK);
    db = NULL;

    CHECK(tsdb_open_readonly(DB_FILE, &db) == TSDB_OK);
    CHECK(verify_model(db, &model) == 0);
    CHECK(verify_extreme_timestamps(db) == 0);
    CHECK(tsdb_close(db) == TSDB_OK);
    CHECK(remove(DB_FILE) == 0);
    puts("Query reference-model tests passed.");
    return 0;
}
