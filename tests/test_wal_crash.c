#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tsdb/tsdb.h"
#include "tsdb/wal.h"
#include "tsdb/wal_format.h"
#include "wal_fault_hooks.h"

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            goto cleanup;                                                                      \
        }                                                                                      \
    } while (0)

enum {
    CHILD_TIMEOUT_MS = 10000,
    CHILD_CRASH_EXIT = 0xE001,
    BASELINE_BATCH_SIZE = TSDB_WAL_BEGIN_V2_SIZE + 2u * TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE,
    PENDING_BATCH_SIZE = TSDB_WAL_BEGIN_V2_SIZE + 3u * TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE,
    BASELINE_FILE_SIZE = TSDB_WAL_FILE_HEADER_SIZE + BASELINE_BATCH_SIZE,
    MAX_FILE_SIZE = BASELINE_FILE_SIZE + PENDING_BATCH_SIZE
};

static const TsdbPoint baseline[] = {{"cpu", 1000, 10.0}, {"ram", 2000, 20.0}};
static const TsdbPoint pending[] = {
    {"cpu", 1000, 99.0}, {"cpu", 500, 5.0}, {"ram", 3000, 30.0}};

typedef struct {
    const char *name;
    size_t bytes_before_pause;
    int after_success;
} CrashScenario;

typedef struct {
    HANDLE ready;
    HANDLE job;
    PROCESS_INFORMATION process;
} ChildProcess;

typedef struct {
    char directory[MAX_PATH];
    char database[MAX_PATH];
    char backup[MAX_PATH];
} Fixture;

static void signal_and_wait(void *context)
{
    HANDLE ready = *(HANDLE *)context;
    if (!SetEvent(ready)) {
        ExitProcess(3u);
    }
    Sleep(INFINITE);
    ExitProcess(4u);
}

static int child_main(const char *event_name, const char *path, const char *checkpoint)
{
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name);
    if (ready == NULL) {
        return 2;
    }

    if (strcmp(checkpoint, "confirmed") == 0) {
        Tsdb *db = NULL;
        if (tsdb_create(path, &db) != TSDB_OK ||
            tsdb_write_batch(db, baseline, 2u) != TSDB_OK ||
            tsdb_write_batch(db, pending, 3u) != TSDB_OK) {
            return 2;
        }
        signal_and_wait(&ready);
    }

    char *end = NULL;
    errno = 0;
    unsigned long prefix = strtoul(checkpoint, &end, 10);
    if (errno != 0 || end == checkpoint || *end != '\0' || prefix > PENDING_BATCH_SIZE) {
        return 2;
    }

    TsdbWal *wal = NULL;
    if (tsdb_wal_create(path, &wal) != TSDB_OK ||
        tsdb_wal_write_batch(wal, baseline, 2u, NULL) != TSDB_OK) {
        return 2;
    }
    tsdb_wal_test_pause_write(wal, (size_t)prefix, signal_and_wait, &ready);
    (void)tsdb_wal_write_batch(wal, pending, 3u, NULL);
    return 2;
}

static int stop_child(ChildProcess *child)
{
    int stopped = 1;
    if (child->process.hProcess != NULL) {
        DWORD state = WaitForSingleObject(child->process.hProcess, 0u);
        if (state == WAIT_TIMEOUT &&
            !TerminateProcess(child->process.hProcess, CHILD_CRASH_EXIT)) {
            stopped = 0;
        }
        DWORD exit_code = 0u;
        if (WaitForSingleObject(child->process.hProcess, CHILD_TIMEOUT_MS) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(child->process.hProcess, &exit_code) ||
            exit_code != CHILD_CRASH_EXIT) {
            stopped = 0;
        }
    }
    if (child->job != NULL) {
        (void)CloseHandle(child->job);
    }
    if (child->process.hThread != NULL) {
        (void)CloseHandle(child->process.hThread);
    }
    if (child->process.hProcess != NULL) {
        (void)CloseHandle(child->process.hProcess);
    }
    if (child->ready != NULL) {
        (void)CloseHandle(child->ready);
    }
    memset(child, 0, sizeof *child);
    return stopped;
}

static int crash_child(const Fixture *fixture, const CrashScenario *scenario, size_t number)
{
    ChildProcess child = {0};
    int crashed = 0;
    char executable[32768];
    char command[32768];
    char event_name[96];
    char checkpoint[32];
    DWORD path_length = GetModuleFileNameA(NULL, executable, sizeof executable);
    CHECK(path_length != 0u && path_length < sizeof executable);
    CHECK(_snprintf_s(event_name, sizeof event_name, _TRUNCATE, "Local\\tsdb-crash-%lu-%zu",
                      (unsigned long)GetCurrentProcessId(), number) > 0);

    child.ready = CreateEventA(NULL, TRUE, FALSE, event_name);
    CHECK(child.ready != NULL && GetLastError() != ERROR_ALREADY_EXISTS);
    child.job = CreateJobObjectW(NULL, NULL);
    CHECK(child.job != NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits,
                                  sizeof limits));

    CHECK(_snprintf_s(checkpoint, sizeof checkpoint, _TRUNCATE, "%zu",
                      scenario->bytes_before_pause) > 0);
    CHECK(_snprintf_s(command, sizeof command, _TRUNCATE, "\"%s\" --child \"%s\" \"%s\" %s",
                      executable, event_name, fixture->database,
                      scenario->after_success ? "confirmed" : checkpoint) > 0);
    STARTUPINFOA startup = {0};
    startup.cb = sizeof startup;
    CHECK(CreateProcessA(executable, command, NULL, NULL, FALSE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &startup,
                         &child.process));
    CHECK(AssignProcessToJobObject(child.job, child.process.hProcess));
    CHECK(ResumeThread(child.process.hThread) != (DWORD)-1);

    HANDLE waiting[] = {child.ready, child.process.hProcess};
    DWORD result = WaitForMultipleObjects(2u, waiting, FALSE, CHILD_TIMEOUT_MS);
    if (result == WAIT_OBJECT_0 + 1u) {
        DWORD exit_code = 0u;
        (void)GetExitCodeProcess(child.process.hProcess, &exit_code);
        fprintf(stderr, "Child exited before checkpoint: %lu\n", (unsigned long)exit_code);
    }
    CHECK(result == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(child.process.hProcess, 0u) == WAIT_TIMEOUT);
    crashed = 1;

cleanup:
    if (!stop_child(&child)) {
        crashed = 0;
    }
    return crashed;
}

static int create_fixture(Fixture *fixture)
{
    char temporary[MAX_PATH];
    DWORD length = GetTempPathA(sizeof temporary, temporary);
    if (length == 0u || length >= sizeof temporary ||
        !GetTempFileNameA(temporary, "tsc", 0u, fixture->directory)) {
        return 0;
    }
    if (!DeleteFileA(fixture->directory) || !CreateDirectoryA(fixture->directory, NULL)) {
        fixture->directory[0] = '\0';
        return 0;
    }
    return _snprintf_s(fixture->database, sizeof fixture->database, _TRUNCATE,
                       "%s\\journal.bin", fixture->directory) > 0 &&
           _snprintf_s(fixture->backup, sizeof fixture->backup, _TRUNCATE,
                       "%s\\before-repair.bin", fixture->directory) > 0;
}

static int remove_if_present(const char *path)
{
    return DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

static int remove_fixture(const Fixture *fixture)
{
    return remove_if_present(fixture->database) && remove_if_present(fixture->backup) &&
           RemoveDirectoryA(fixture->directory);
}

static int load_file(const char *path, uint8_t bytes[MAX_FILE_SIZE], size_t *length)
{
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb") != 0) {
        return 0;
    }
    size_t read = fread(bytes, 1u, MAX_FILE_SIZE, file);
    int complete = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) {
        complete = 0;
    }
    if (complete) {
        *length = read;
    }
    return complete;
}

static int verify_metric(Tsdb *db, const char *metric, const TsdbPoint *expected,
                         size_t expected_count, TsdbAggregate expected_aggregate)
{
    int correct = 0;
    TsdbPoint *points = NULL;
    size_t count = 0u;
    CHECK(tsdb_query_range(db, metric, INT64_MIN, INT64_MAX, &points, &count) == TSDB_OK);
    CHECK(count == expected_count);
    for (size_t i = 0u; i < count; ++i) {
        CHECK(strcmp(points[i].metric, expected[i].metric) == 0);
        CHECK(points[i].timestamp_ms == expected[i].timestamp_ms);
        CHECK(points[i].value == expected[i].value);
    }
    TsdbAggregate aggregate = {0};
    CHECK(tsdb_aggregate(db, metric, INT64_MIN, INT64_MAX, &aggregate) == TSDB_OK);
    CHECK(aggregate.count == expected_aggregate.count);
    CHECK(aggregate.min == expected_aggregate.min && aggregate.max == expected_aggregate.max);
    CHECK(aggregate.avg == expected_aggregate.avg);
    correct = 1;

cleanup:
    tsdb_free_points(points);
    return correct;
}

static int verify_recovered(const char *path, int includes_pending)
{
    static const TsdbPoint cpu[] = {{"cpu", 500, 5.0}, {"cpu", 1000, 99.0}};
    static const TsdbPoint ram[] = {{"ram", 2000, 20.0}, {"ram", 3000, 30.0}};
    Tsdb *db = NULL;
    int correct = 0;
    CHECK(tsdb_open_readonly(path, &db) == TSDB_OK);
    if (includes_pending) {
        CHECK(verify_metric(db, "cpu", cpu, 2u, (TsdbAggregate){2u, 5.0, 99.0, 52.0}));
        CHECK(verify_metric(db, "ram", ram, 2u, (TsdbAggregate){2u, 20.0, 30.0, 25.0}));
    } else {
        CHECK(
            verify_metric(db, "cpu", &baseline[0], 1u, (TsdbAggregate){1u, 10.0, 10.0, 10.0}));
        CHECK(
            verify_metric(db, "ram", &baseline[1], 1u, (TsdbAggregate){1u, 20.0, 20.0, 20.0}));
    }
    correct = 1;

cleanup:
    if (db != NULL && tsdb_close(db) != TSDB_OK) {
        correct = 0;
    }
    return correct;
}

static int append_after_recovery(const char *path)
{
    Tsdb *db = NULL;
    int appended = 0;
    CHECK(tsdb_open(path, &db) == TSDB_OK);
    CHECK(tsdb_write_batch(db, pending, 3u) == TSDB_OK);
    appended = 1;

cleanup:
    if (db != NULL && tsdb_close(db) != TSDB_OK) {
        appended = 0;
    }
    return appended && verify_recovered(path, 1);
}

static int run_scenario(const CrashScenario *scenario, size_t number)
{
    Fixture fixture = {0};
    Tsdb *db = NULL;
    int passed = 0;
    uint8_t before[MAX_FILE_SIZE];
    uint8_t after[MAX_FILE_SIZE];
    size_t before_length = 0u;
    size_t after_length = 0u;
    size_t appended_bytes =
        scenario->after_success ? PENDING_BATCH_SIZE : scenario->bytes_before_pause;
    int incomplete = appended_bytes != 0u && appended_bytes < PENDING_BATCH_SIZE;

    CHECK(create_fixture(&fixture));
    CHECK(crash_child(&fixture, scenario, number));
    CHECK(load_file(fixture.database, before, &before_length));
    CHECK(before_length == BASELINE_FILE_SIZE + appended_bytes);
    TsdbStatus open_status = tsdb_open_readonly(fixture.database, &db);
    CHECK(open_status == (incomplete ? TSDB_ERR_INCOMPLETE : TSDB_OK));
    if (incomplete) {
        CHECK(db == NULL);
    } else {
        TsdbStatus close_status = tsdb_close(db);
        db = NULL;
        CHECK(close_status == TSDB_OK);
    }
    CHECK(load_file(fixture.database, after, &after_length));
    CHECK(after_length == before_length && memcmp(before, after, before_length) == 0);

    uint64_t discarded = UINT64_MAX;
    CHECK(tsdb_repair(fixture.database, fixture.backup, &discarded) == TSDB_OK);
    if (incomplete) {
        CHECK(discarded == appended_bytes);
        CHECK(load_file(fixture.backup, after, &after_length));
        CHECK(after_length == before_length && memcmp(before, after, before_length) == 0);
        CHECK(load_file(fixture.database, after, &after_length));
        CHECK(after_length == BASELINE_FILE_SIZE);
        CHECK(memcmp(before, after, after_length) == 0);
    } else {
        CHECK(discarded == 0u);
        CHECK(GetFileAttributesA(fixture.backup) == INVALID_FILE_ATTRIBUTES &&
              GetLastError() == ERROR_FILE_NOT_FOUND);
        CHECK(load_file(fixture.database, after, &after_length));
        CHECK(after_length == before_length && memcmp(before, after, before_length) == 0);
    }

    int includes_pending = appended_bytes == PENDING_BATCH_SIZE;
    CHECK(verify_recovered(fixture.database, includes_pending));
    if (!includes_pending) {
        CHECK(append_after_recovery(fixture.database));
    }
    CHECK(remove_fixture(&fixture));
    passed = 1;

cleanup:
    if (db != NULL) {
        (void)tsdb_close(db);
    }
    if (!passed && fixture.directory[0] != '\0') {
        fprintf(stderr, "Scenario '%s'; files retained in: %s\n", scenario->name,
                fixture.directory);
    }
    if (passed) {
        printf("PASS: %s\n", scenario->name);
    }
    return passed;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--child") == 0) {
        return child_main(argv[2], argv[3], argv[4]);
    }
    if (argc != 1) {
        return 2;
    }

    const CrashScenario scenarios[] = {
        {"after write_batch success", PENDING_BATCH_SIZE, 1},
        {"before the next BEGIN", 0u, 0},
        {"during BEGIN", 7u, 0},
        {"during the first record", TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE / 2u, 0},
        {"after a complete replacement record", TSDB_WAL_BEGIN_V2_SIZE + TSDB_RECORD_SIZE, 0},
        {"before COMMIT", PENDING_BATCH_SIZE - TSDB_WAL_COMMIT_SIZE, 0},
        {"during COMMIT", PENDING_BATCH_SIZE - TSDB_WAL_COMMIT_SIZE / 2u, 0},
        {"after COMMIT, before flush and success", PENDING_BATCH_SIZE, 0}};
    size_t count = sizeof scenarios / sizeof scenarios[0];
    for (size_t i = 0u; i < count; ++i) {
        if (!run_scenario(&scenarios[i], i)) {
            return 1;
        }
    }
    printf("All %zu WAL process crash scenarios passed.\n", count);
    return 0;
}
