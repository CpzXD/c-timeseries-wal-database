#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tsdb/tsdb.h"

static const char *status_text(TsdbStatus status)
{
    switch (status) {
    case TSDB_OK:
        return "ok";
    case TSDB_ERR_ARGUMENT:
        return "argument invalid";
    case TSDB_ERR_FORMAT:
        return "format de fisier invalid";
    case TSDB_ERR_CHECKSUM:
        return "checksum diferit; date corupte";
    case TSDB_ERR_IO:
        return "eroare de intrare/iesire";
    case TSDB_ERR_MEMORY:
        return "memorie insuficienta";
    case TSDB_ERR_LIMIT:
        return "limita depasita";
    case TSDB_ERR_INCOMPLETE:
        return "coada WAL incompleta; foloseste repair cu backup";
    case TSDB_ERR_READ_ONLY:
        return "baza a fost deschisa doar pentru citire";
    case TSDB_ERR_EXISTS:
        return "fisierul exista deja";
    case TSDB_ERR_NOT_FOUND:
        return "fisierul nu exista; foloseste init";
    case TSDB_ERR_NOT_IMPLEMENTED:
        return "neimplementat";
    default:
        return "eroare necunoscuta";
    }
}

static int print_error(const char *operation, TsdbStatus status)
{
    fprintf(stderr, "%s: %s\n", operation, status_text(status));
    return 1;
}

static int close_after_operation(Tsdb *db, const char *operation, TsdbStatus status)
{
    TsdbStatus close_status = tsdb_close(db);
    if (status != TSDB_OK) {
        return print_error(operation, status);
    }
    if (close_status != TSDB_OK) {
        return print_error("close", close_status);
    }
    return 0;
}

static int parse_timestamp(const char *text, int64_t *out)
{
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return 0;
    }
    *out = (int64_t)value;
    return 1;
}

static int parse_value(const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0') {
        return 0;
    }
    *out = value;
    return 1;
}

static int run_init(const char *path)
{
    Tsdb *db = NULL;
    TsdbStatus status = tsdb_create(path, &db);
    if (status != TSDB_OK) {
        return print_error("init", status);
    }
    if (close_after_operation(db, "init", TSDB_OK) != 0) {
        return 1;
    }
    puts("Baza de date este pregatita.");
    return 0;
}

static int run_repair(const char *path, const char *backup_path)
{
    uint64_t discarded = 0u;
    TsdbStatus status = tsdb_repair(path, backup_path, &discarded);
    if (status != TSDB_OK) {
        return print_error("repair", status);
    }
    printf("Octeti eliminati: %" PRIu64 "\n", discarded);
    return 0;
}

static int run_write(const char *path, const char *metric, const char *timestamp_text,
                     const char *value_text)
{
    TsdbPoint point = {0};
    size_t metric_length = strlen(metric);
    if (metric_length == 0u || metric_length >= TSDB_METRIC_CAPACITY ||
        !parse_timestamp(timestamp_text, &point.timestamp_ms) ||
        !parse_value(value_text, &point.value)) {
        return print_error("write", TSDB_ERR_ARGUMENT);
    }
    memcpy(point.metric, metric, metric_length + 1u);

    Tsdb *db = NULL;
    TsdbStatus status = tsdb_open(path, &db);
    if (status != TSDB_OK) {
        return print_error("open", status);
    }
    status = tsdb_write_batch(db, &point, 1u);
    if (close_after_operation(db, "write", status) != 0) {
        return 1;
    }
    puts("Punct confirmat in WAL.");
    return 0;
}

static TsdbStatus print_query(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms)
{
    TsdbPoint *points = NULL;
    size_t count = 0u;
    TsdbStatus status = tsdb_query_range(db, metric, from_ms, to_ms, &points, &count);
    if (status == TSDB_OK) {
        for (size_t point_index = 0u; point_index < count; ++point_index) {
            const TsdbPoint *point = &points[point_index];
            printf("%s %" PRId64 " %.17g\n", point->metric, point->timestamp_ms, point->value);
        }
    }
    tsdb_free_points(points);
    return status;
}

static TsdbStatus print_aggregate(Tsdb *db, const char *metric, int64_t from_ms, int64_t to_ms)
{
    TsdbAggregate aggregate = {0};
    TsdbStatus status = tsdb_aggregate(db, metric, from_ms, to_ms, &aggregate);
    if (status == TSDB_OK) {
        printf("count=%zu min=%.17g max=%.17g avg=%.17g\n", aggregate.count, aggregate.min,
               aggregate.max, aggregate.avg);
    }
    return status;
}

static int run_read(const char *command, const char *path, const char *metric,
                    const char *from_text, const char *to_text)
{
    int64_t from_ms = 0;
    int64_t to_ms = 0;
    if (!parse_timestamp(from_text, &from_ms) || !parse_timestamp(to_text, &to_ms)) {
        return print_error(command, TSDB_ERR_ARGUMENT);
    }

    Tsdb *db = NULL;
    TsdbStatus status = tsdb_open_readonly(path, &db);
    if (status != TSDB_OK) {
        return print_error("open", status);
    }
    if (strcmp(command, "query") == 0) {
        status = print_query(db, metric, from_ms, to_ms);
    } else {
        status = print_aggregate(db, metric, from_ms, to_ms);
    }
    return close_after_operation(db, command, status);
}

static void print_usage(void)
{
    puts("Utilizare:");
    puts("  tsdb --version");
    puts("  tsdb init <fisier>");
    puts("  tsdb repair <fisier> <backup_nou>");
    puts("  tsdb write <fisier> <metrica> <timestamp_ms> <valoare>");
    puts("  tsdb query <fisier> <metrica> <de_la_ms> <pana_la_ms>");
    puts("  tsdb aggregate <fisier> <metrica> <de_la_ms> <pana_la_ms>");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("tsdb-c 0.1.0");
        return 0;
    }
    if (argc < 3) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];
    if (strcmp(command, "init") == 0 && argc == 3) {
        return run_init(argv[2]);
    }
    if (strcmp(command, "repair") == 0 && argc == 4) {
        return run_repair(argv[2], argv[3]);
    }
    if (strcmp(command, "write") == 0 && argc == 6) {
        return run_write(argv[2], argv[3], argv[4], argv[5]);
    }
    if ((strcmp(command, "query") == 0 || strcmp(command, "aggregate") == 0) && argc == 6) {
        return run_read(command, argv[2], argv[3], argv[4], argv[5]);
    }

    print_usage();
    return 1;
}
