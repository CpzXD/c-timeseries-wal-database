#include <math.h>
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

#define DB_FILE "query_test.bin"

static int verify_results(Tsdb *db)
{
    TsdbPoint *results = NULL;
    size_t count = 99u;
    CHECK(tsdb_query_range(db, "cpu", -100, 300, &results, &count) == TSDB_OK);
    CHECK(count == 4u && results != NULL);
    CHECK(results[0].timestamp_ms == -100 && results[0].value == 5.0);
    CHECK(results[1].timestamp_ms == 100 && results[1].value == 10.0);
    CHECK(results[2].timestamp_ms == 200 && results[2].value == 25.0);
    CHECK(results[3].timestamp_ms == 300 && results[3].value == 30.0);
    tsdb_free_points(results);

    TsdbAggregate aggregate = {0};
    CHECK(tsdb_aggregate(db, "cpu", -100, 300, &aggregate) == TSDB_OK);
    CHECK(aggregate.count == 4u && aggregate.min == 5.0 && aggregate.max == 30.0 &&
          aggregate.avg == 17.5);

    CHECK(tsdb_query_range(db, "cpu", 200, 200, &results, &count) == TSDB_OK);
    CHECK(count == 1u && results[0].value == 25.0);
    tsdb_free_points(results);

    CHECK(tsdb_query_range(db, "missing", 0, 1000, &results, &count) == TSDB_OK);
    CHECK(count == 0u && results == NULL);
    CHECK(tsdb_aggregate(db, "missing", 0, 1000, &aggregate) == TSDB_OK);
    CHECK(aggregate.count == 0u && aggregate.min == 0.0 && aggregate.max == 0.0 &&
          aggregate.avg == 0.0);

    CHECK(tsdb_query_range(db, "ram", 0, 1000, &results, &count) == TSDB_OK);
    CHECK(count == 1u && results[0].timestamp_ms == 150 && results[0].value == 65.0);
    tsdb_free_points(results);

    CHECK(tsdb_query_range(db, "cpu", 300, 100, &results, &count) == TSDB_ERR_ARGUMENT);
    CHECK(tsdb_query_range(db, "bad!", 0, 100, &results, &count) == TSDB_ERR_ARGUMENT);
    return 0;
}

static int verify_disk(Tsdb *db)
{
    TsdbPoint *results = NULL;
    size_t count = 0u;
    CHECK(tsdb_query_range(db, "disk", 0, 49, &results, &count) == TSDB_OK);
    CHECK(count == 50u);
    for (size_t timestamp = 0u; timestamp < 50u; ++timestamp) {
        size_t first_write = 0u;
        while ((first_write * 37u) % 50u != timestamp) {
            ++first_write;
        }
        CHECK(results[timestamp].timestamp_ms == (int64_t)timestamp);
        CHECK(results[timestamp].value == (double)(50u + first_write));
    }
    tsdb_free_points(results);

    TsdbAggregate aggregate = {0};
    CHECK(tsdb_aggregate(db, "disk", 0, 49, &aggregate) == TSDB_OK);
    CHECK(aggregate.count == 50u && aggregate.min == 50.0 && aggregate.max == 99.0 &&
          fabs(aggregate.avg - 74.5) < 1e-12);
    return 0;
}

int main(void)
{
    (void)remove(DB_FILE);
    Tsdb *db = NULL;
    CHECK(tsdb_create(DB_FILE, &db) == TSDB_OK);

    TsdbPoint first[3] = {{{0}, 200, 20.0}, {{0}, 150, 60.0}, {{0}, 100, 10.0}};
    memcpy(first[0].metric, "cpu", sizeof "cpu");
    memcpy(first[1].metric, "ram", sizeof "ram");
    memcpy(first[2].metric, "cpu", sizeof "cpu");
    CHECK(tsdb_write_batch(db, first, 3u) == TSDB_OK);

    TsdbPoint second[4] = {
        {{0}, 200, 25.0}, {{0}, -100, 5.0}, {{0}, 300, 30.0}, {{0}, 150, 65.0}};
    for (size_t i = 0u; i < 3u; ++i) {
        memcpy(second[i].metric, "cpu", sizeof "cpu");
    }
    memcpy(second[3].metric, "ram", sizeof "ram");
    CHECK(tsdb_write_batch(db, second, 4u) == TSDB_OK);

    TsdbPoint invalid = {{0}, 0, 1.0};
    memcpy(invalid.metric, "bad!", sizeof "bad!");
    CHECK(tsdb_write_batch(db, &invalid, 1u) == TSDB_ERR_ARGUMENT);
    CHECK(verify_results(db) == 0);

    TsdbPoint disk[100] = {0};
    for (size_t i = 0u; i < 100u; ++i) {
        memcpy(disk[i].metric, "disk", sizeof "disk");
        disk[i].timestamp_ms = (int64_t)((i * 37u) % 50u);
        disk[i].value = (double)i;
    }
    CHECK(tsdb_write_batch(db, disk, 100u) == TSDB_OK);
    CHECK(verify_disk(db) == 0);
    CHECK(tsdb_close(db) == TSDB_OK);
    db = NULL;

    CHECK(tsdb_open_readonly(DB_FILE, &db) == TSDB_OK);
    CHECK(verify_results(db) == 0);
    CHECK(verify_disk(db) == 0);
    CHECK(tsdb_close(db) == TSDB_OK);
    CHECK(remove(DB_FILE) == 0);
    puts("Query and aggregate tests passed.");
    return 0;
}
