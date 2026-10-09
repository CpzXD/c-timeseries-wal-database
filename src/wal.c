#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "tsdb/wal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tsdb/crc32.h"
#include "tsdb/wal_format.h"

#ifdef TSDB_WAL_FAULT_TEST
#include "wal_fault_hooks.h"

enum { WAL_FAULT_NONE, WAL_FAULT_WRITE, WAL_FAULT_FLUSH, WAL_FAULT_PAUSE_WRITE };
#endif

struct TsdbWal {
    HANDLE file;
    uint64_t end_offset;
    uint64_t next_sequence;
    int format_version;
    int writable;
    int failed;
#ifdef TSDB_WAL_FAULT_TEST
    int test_fault;
    size_t test_write_prefix;
    int test_fail_rollback;
    TsdbWalTestCheckpoint test_checkpoint;
    void *test_checkpoint_context;
#endif
};

#ifdef TSDB_WAL_FAULT_TEST
void tsdb_wal_test_fail_write(TsdbWal *wal, size_t bytes_before_failure, int fail_rollback)
{
    wal->test_fault = WAL_FAULT_WRITE;
    wal->test_write_prefix = bytes_before_failure;
    wal->test_fail_rollback = fail_rollback;
}

void tsdb_wal_test_fail_flush(TsdbWal *wal, int fail_rollback)
{
    wal->test_fault = WAL_FAULT_FLUSH;
    wal->test_write_prefix = 0u;
    wal->test_fail_rollback = fail_rollback;
}

void tsdb_wal_test_pause_write(TsdbWal *wal, size_t bytes_before_pause,
                               TsdbWalTestCheckpoint checkpoint, void *context)
{
    wal->test_fault = WAL_FAULT_PAUSE_WRITE;
    wal->test_write_prefix = bytes_before_pause;
    wal->test_checkpoint = checkpoint;
    wal->test_checkpoint_context = context;
}
#endif

static int seek_to(HANDLE file, uint64_t offset)
{
    LARGE_INTEGER position;
    if (offset > LLONG_MAX) {
        return 0;
    }
    position.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) != 0;
}

static int read_at(HANDLE file, uint64_t offset, uint8_t *buffer, size_t length)
{
    if (!seek_to(file, offset)) {
        return 0;
    }
    while (length != 0u) {
        DWORD amount = length > 0x40000000u ? 0x40000000u : (DWORD)length;
        DWORD got = 0u;
        if (!ReadFile(file, buffer, amount, &got, NULL) || got == 0u) {
            return 0;
        }
        buffer += got;
        length -= got;
    }
    return 1;
}

static int write_all(HANDLE file, const uint8_t *buffer, size_t length)
{
    while (length != 0u) {
        DWORD amount = length > 0x40000000u ? 0x40000000u : (DWORD)length;
        DWORD written = 0u;
        if (!WriteFile(file, buffer, amount, &written, NULL) || written == 0u) {
            return 0;
        }
        buffer += written;
        length -= written;
    }
    return 1;
}

static int truncate_to(HANDLE file, uint64_t offset)
{
    return seek_to(file, offset) && SetEndOfFile(file) && FlushFileBuffers(file);
}

static int append_write_all(TsdbWal *wal, const uint8_t *buffer, size_t length)
{
#ifdef TSDB_WAL_FAULT_TEST
    if (wal->test_fault == WAL_FAULT_WRITE) {
        size_t prefix = wal->test_write_prefix < length ? wal->test_write_prefix : length;
        wal->test_fault = WAL_FAULT_NONE;
        if (prefix != 0u) {
            (void)write_all(wal->file, buffer, prefix);
        }
        return 0;
    }
    if (wal->test_fault == WAL_FAULT_PAUSE_WRITE) {
        size_t prefix = wal->test_write_prefix < length ? wal->test_write_prefix : length;
        wal->test_fault = WAL_FAULT_NONE;
        if (!write_all(wal->file, buffer, prefix)) {
            return 0;
        }
        wal->test_checkpoint(wal->test_checkpoint_context);
        return write_all(wal->file, buffer + prefix, length - prefix);
    }
#endif
    return write_all(wal->file, buffer, length);
}

static int append_flush(TsdbWal *wal)
{
#ifdef TSDB_WAL_FAULT_TEST
    if (wal->test_fault == WAL_FAULT_FLUSH) {
        wal->test_fault = WAL_FAULT_NONE;
        return 0;
    }
#endif
    return FlushFileBuffers(wal->file) != 0;
}

static int append_rollback(TsdbWal *wal)
{
#ifdef TSDB_WAL_FAULT_TEST
    if (wal->test_fail_rollback) {
        return 0;
    }
#endif
    return truncate_to(wal->file, wal->end_offset);
}

static TsdbStatus detect_format(HANDLE file, int *version, uint64_t *first_batch_offset)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        return TSDB_ERR_IO;
    }
    if (size.QuadPart < 4) {
        return TSDB_ERR_FORMAT;
    }
    uint8_t magic[8] = {0};
    size_t prefix = size.QuadPart < 8 ? (size_t)size.QuadPart : 8u;
    if (!read_at(file, 0u, magic, prefix)) {
        return TSDB_ERR_IO;
    }
    if (prefix == 8u && memcmp(magic, "TSDBWAL2", 8u) == 0) {
        if (size.QuadPart < TSDB_WAL_FILE_HEADER_SIZE) {
            return TSDB_ERR_FORMAT;
        }
        uint8_t header[TSDB_WAL_FILE_HEADER_SIZE];
        if (!read_at(file, 0u, header, sizeof header)) {
            return TSDB_ERR_IO;
        }
        TsdbStatus status = tsdb_wal_file_header_decode(header);
        if (status != TSDB_OK) {
            return status;
        }
        *version = 2;
        *first_batch_offset = TSDB_WAL_FILE_HEADER_SIZE;
        return TSDB_OK;
    }
    if (memcmp(magic, "TBG1", 4u) == 0) {
        *version = 1;
        *first_batch_offset = 0u;
        return TSDB_OK;
    }
    return TSDB_ERR_FORMAT;
}

static TsdbStatus validate_batch_commit(const uint8_t *batch, size_t commit_offset,
                                        uint64_t sequence)
{
    uint64_t commit_sequence = 0u;
    uint32_t stored_crc = 0u;
    TsdbStatus status =
        tsdb_wal_commit_decode(batch + commit_offset, &commit_sequence, &stored_crc);
    if (status != TSDB_OK) {
        return status;
    }
    if (commit_sequence != sequence) {
        return TSDB_ERR_FORMAT;
    }

    uint32_t crc_state = tsdb_crc32_start();
    crc_state = tsdb_crc32_update(crc_state, batch, commit_offset);
    return tsdb_crc32_finish(crc_state) == stored_crc ? TSDB_OK : TSDB_ERR_CHECKSUM;
}

static TsdbStatus replay_batch_points(const uint8_t *batch, size_t begin_size, uint32_t count,
                                      uint64_t sequence, TsdbWalBatchVisitor visitor,
                                      void *context)
{
    TsdbPoint *points = NULL;
    if (visitor != NULL) {
        points = malloc((size_t)count * sizeof *points);
        if (points == NULL) {
            return TSDB_ERR_MEMORY;
        }
    }

    TsdbStatus status = TSDB_OK;
    for (uint32_t point_index = 0u; point_index < count; ++point_index) {
        TsdbPoint temporary;
        TsdbPoint *target = points != NULL ? &points[point_index] : &temporary;
        size_t record_offset = begin_size + (size_t)point_index * TSDB_RECORD_SIZE;
        status = tsdb_record_decode(batch + record_offset, target);
        if (status != TSDB_OK) {
            break;
        }
    }
    if (status == TSDB_OK && visitor != NULL) {
        status = visitor(points, count, sequence, context);
    }

    free(points);
    return status;
}

static TsdbStatus scan_journal(HANDLE file, TsdbWalBatchVisitor visitor, void *context,
                               int version, uint64_t first_batch_offset, uint64_t *valid_end,
                               uint64_t *next_sequence, size_t *batch_count,
                               int *incomplete_tail)
{
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) {
        return TSDB_ERR_IO;
    }

    const uint64_t size = (uint64_t)file_size.QuadPart;
    uint64_t offset = first_batch_offset;
    uint64_t expected_sequence = 1u;
    size_t completed = 0u;
    *incomplete_tail = 0;
    size_t begin_size = version == 2 ? TSDB_WAL_BEGIN_V2_SIZE : TSDB_WAL_BEGIN_SIZE;

    while (offset < size) {
        uint64_t remaining = size - offset;
        if (remaining < begin_size) {
            *incomplete_tail = 1;
            break;
        }

        uint8_t begin[TSDB_WAL_BEGIN_V2_SIZE];
        if (!read_at(file, offset, begin, begin_size)) {
            return TSDB_ERR_IO;
        }

        uint32_t count = 0u;
        uint64_t sequence = 0u;
        TsdbStatus status = version == 2 ? tsdb_wal_begin_v2_decode(begin, &count, &sequence)
                                         : tsdb_wal_begin_decode(begin, &count, &sequence);
        if (status != TSDB_OK) {
            return status;
        }
        if (count > TSDB_WAL_MAX_BATCH_POINTS) {
            return TSDB_ERR_LIMIT;
        }
        if (sequence != expected_sequence || sequence == UINT64_MAX) {
            return TSDB_ERR_FORMAT;
        }

        size_t batch_size =
            begin_size + (size_t)count * TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE;
        if (remaining < batch_size) {
            *incomplete_tail = 1;
            break;
        }

        uint8_t *batch = (uint8_t *)malloc(batch_size);
        if (batch == NULL) {
            return TSDB_ERR_MEMORY;
        }
        if (!read_at(file, offset, batch, batch_size)) {
            free(batch);
            return TSDB_ERR_IO;
        }

        const size_t commit_offset = batch_size - TSDB_WAL_COMMIT_SIZE;
        status = validate_batch_commit(batch, commit_offset, sequence);
        if (status == TSDB_OK) {
            status = replay_batch_points(batch, begin_size, count, sequence, visitor, context);
        }
        free(batch);
        if (status != TSDB_OK) {
            return status;
        }

        offset += batch_size;
        ++expected_sequence;
        ++completed;
    }

    *valid_end = offset;
    *next_sequence = expected_sequence;
    *batch_count = completed;
    return TSDB_OK;
}

static TsdbStatus make_wal(HANDLE file, uint64_t end_offset, uint64_t next_sequence,
                           int version, int writable, TsdbWal **out)
{
    TsdbWal *wal = (TsdbWal *)malloc(sizeof *wal);
    if (wal == NULL) {
        CloseHandle(file);
        return TSDB_ERR_MEMORY;
    }
    wal->file = file;
    wal->end_offset = end_offset;
    wal->next_sequence = next_sequence;
    wal->format_version = version;
    wal->writable = writable;
    wal->failed = 0;
#ifdef TSDB_WAL_FAULT_TEST
    wal->test_fault = WAL_FAULT_NONE;
    wal->test_write_prefix = 0u;
    wal->test_fail_rollback = 0;
    wal->test_checkpoint = NULL;
    wal->test_checkpoint_context = NULL;
#endif
    *out = wal;
    return TSDB_OK;
}

TsdbStatus tsdb_wal_create(const char *path, TsdbWal **out)
{
    if (path == NULL || path[0] == '\0' || out == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    *out = NULL;
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? TSDB_ERR_EXISTS
                                                                           : TSDB_ERR_IO;
    }
    uint8_t header[TSDB_WAL_FILE_HEADER_SIZE];
    TsdbStatus status = tsdb_wal_file_header_encode(header);
    if (status != TSDB_OK || !write_all(file, header, sizeof header) ||
        !FlushFileBuffers(file)) {
        CloseHandle(file);
        (void)DeleteFileA(path);
        return TSDB_ERR_IO;
    }
    return make_wal(file, sizeof header, 1u, 2, 1, out);
}

TsdbStatus tsdb_wal_open(const char *path, TsdbWalMode mode, TsdbWal **out)
{
    if (path == NULL || path[0] == '\0' || out == NULL ||
        (mode != TSDB_WAL_READ_ONLY && mode != TSDB_WAL_READ_WRITE)) {
        return TSDB_ERR_ARGUMENT;
    }
    *out = NULL;
    int writable = mode == TSDB_WAL_READ_WRITE;
    HANDLE file = CreateFileA(path, GENERIC_READ | (writable ? GENERIC_WRITE : 0u),
                              writable ? 0u : FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? TSDB_ERR_NOT_FOUND : TSDB_ERR_IO;
    }

    int version = 0;
    uint64_t first_batch_offset = 0u;
    TsdbStatus status = detect_format(file, &version, &first_batch_offset);
    uint64_t valid_end = 0u;
    uint64_t next_sequence = 1u;
    size_t batch_count = 0u;
    int incomplete_tail = 0;
    if (status == TSDB_OK) {
        status = scan_journal(file, NULL, NULL, version, first_batch_offset, &valid_end,
                              &next_sequence, &batch_count, &incomplete_tail);
    }
    if (status == TSDB_OK && incomplete_tail) {
        status = TSDB_ERR_INCOMPLETE;
    }
    if (status != TSDB_OK) {
        CloseHandle(file);
        return status;
    }
    return make_wal(file, valid_end, next_sequence, version, writable, out);
}

static TsdbStatus backup_to_new_file(HANDLE source, const char *backup_path)
{
    HANDLE backup = CreateFileA(backup_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (backup == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? TSDB_ERR_EXISTS
                                                                           : TSDB_ERR_IO;
    }
    LARGE_INTEGER size;
    TsdbStatus status = TSDB_OK;
    uint8_t *buffer = (uint8_t *)malloc(65536u);
    if (buffer == NULL) {
        status = TSDB_ERR_MEMORY;
    } else if (!GetFileSizeEx(source, &size) || size.QuadPart < 0) {
        status = TSDB_ERR_IO;
    } else {
        uint64_t offset = 0u;
        while (offset < (uint64_t)size.QuadPart) {
            size_t amount = (uint64_t)size.QuadPart - offset > 65536u
                                ? 65536u
                                : (size_t)((uint64_t)size.QuadPart - offset);
            if (!read_at(source, offset, buffer, amount) ||
                !write_all(backup, buffer, amount)) {
                status = TSDB_ERR_IO;
                break;
            }
            offset += amount;
        }
        if (status == TSDB_OK && !FlushFileBuffers(backup)) {
            status = TSDB_ERR_IO;
        }
    }
    free(buffer);
    if (!CloseHandle(backup)) {
        status = TSDB_ERR_IO;
    }
    if (status != TSDB_OK) {
        (void)DeleteFileA(backup_path);
    }
    return status;
}

TsdbStatus tsdb_wal_repair(const char *path, const char *backup_path,
                           uint64_t *out_discarded_bytes)
{
    if (path == NULL || path[0] == '\0' || backup_path == NULL || backup_path[0] == '\0') {
        return TSDB_ERR_ARGUMENT;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? TSDB_ERR_NOT_FOUND : TSDB_ERR_IO;
    }
    int version = 0;
    uint64_t first_batch_offset = 0u;
    TsdbStatus status = detect_format(file, &version, &first_batch_offset);
    uint64_t valid_end = 0u;
    uint64_t next_sequence = 1u;
    size_t batch_count = 0u;
    int incomplete_tail = 0;
    if (status == TSDB_OK) {
        status = scan_journal(file, NULL, NULL, version, first_batch_offset, &valid_end,
                              &next_sequence, &batch_count, &incomplete_tail);
    }
    if (status == TSDB_OK && incomplete_tail) {
        LARGE_INTEGER size;
        if (version == 1) {

            status = TSDB_ERR_FORMAT;
        } else if (!GetFileSizeEx(file, &size)) {
            status = TSDB_ERR_IO;
        } else {
            status = backup_to_new_file(file, backup_path);
            if (status == TSDB_OK && !truncate_to(file, valid_end)) {
                status = TSDB_ERR_IO;
            }
            if (status == TSDB_OK && out_discarded_bytes != NULL) {
                *out_discarded_bytes = (uint64_t)size.QuadPart - valid_end;
            }
        }
    } else if (status == TSDB_OK && out_discarded_bytes != NULL) {
        *out_discarded_bytes = 0u;
    }
    if (!CloseHandle(file) && status == TSDB_OK) {
        status = TSDB_ERR_IO;
    }
    return status;
}

TsdbStatus tsdb_wal_write_batch(TsdbWal *wal, const TsdbPoint *points, size_t count,
                                uint64_t *out_sequence)
{
    if (wal == NULL || points == NULL || count == 0u) {
        return TSDB_ERR_ARGUMENT;
    }
    if (wal->failed) {
        return TSDB_ERR_IO;
    }
    if (!wal->writable) {
        return TSDB_ERR_READ_ONLY;
    }
    if (count > TSDB_WAL_MAX_BATCH_POINTS || wal->next_sequence == UINT64_MAX) {
        return TSDB_ERR_LIMIT;
    }

    size_t begin_size = wal->format_version == 2 ? TSDB_WAL_BEGIN_V2_SIZE : TSDB_WAL_BEGIN_SIZE;
    size_t batch_size = begin_size + count * TSDB_RECORD_SIZE + TSDB_WAL_COMMIT_SIZE;
    uint8_t *batch = (uint8_t *)malloc(batch_size);
    if (batch == NULL) {
        return TSDB_ERR_MEMORY;
    }

    const uint64_t sequence = wal->next_sequence;
    TsdbStatus status = wal->format_version == 2
                            ? tsdb_wal_begin_v2_encode((uint32_t)count, sequence, batch)
                            : tsdb_wal_begin_encode((uint32_t)count, sequence, batch);
    for (size_t i = 0u; status == TSDB_OK && i < count; ++i) {
        status = tsdb_record_encode(&points[i], batch + begin_size + i * TSDB_RECORD_SIZE);
    }
    if (status != TSDB_OK) {
        free(batch);
        return status;
    }

    const size_t commit_offset = batch_size - TSDB_WAL_COMMIT_SIZE;
    uint32_t crc_state = tsdb_crc32_start();
    crc_state = tsdb_crc32_update(crc_state, batch, commit_offset);
    uint32_t batch_crc = tsdb_crc32_finish(crc_state);
    status = tsdb_wal_commit_encode(sequence, batch_crc, batch + commit_offset);
    if (status != TSDB_OK) {
        free(batch);
        return status;
    }

    int persisted = seek_to(wal->file, wal->end_offset) &&
                    append_write_all(wal, batch, batch_size) && append_flush(wal);
    free(batch);
    if (!persisted) {

        wal->failed = 1;
        (void)append_rollback(wal);
        return TSDB_ERR_IO;
    }

    wal->end_offset += batch_size;
    ++wal->next_sequence;
    if (out_sequence != NULL) {
        *out_sequence = sequence;
    }
    return TSDB_OK;
}

TsdbStatus tsdb_wal_replay(TsdbWal *wal, TsdbWalBatchVisitor visitor, void *context,
                           size_t *out_batch_count)
{
    if (wal == NULL || visitor == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    if (wal->failed) {
        return TSDB_ERR_IO;
    }
    uint64_t valid_end = 0u;
    uint64_t next_sequence = 1u;
    size_t batch_count = 0u;
    int incomplete_tail = 0;
    uint64_t first_batch_offset = wal->format_version == 2 ? TSDB_WAL_FILE_HEADER_SIZE : 0u;
    TsdbStatus status =
        scan_journal(wal->file, visitor, context, wal->format_version, first_batch_offset,
                     &valid_end, &next_sequence, &batch_count, &incomplete_tail);
    if (status != TSDB_OK) {
        return status;
    }
    if (incomplete_tail || valid_end != wal->end_offset ||
        next_sequence != wal->next_sequence) {
        return TSDB_ERR_FORMAT;
    }
    if (out_batch_count != NULL) {
        *out_batch_count = batch_count;
    }
    return TSDB_OK;
}

TsdbStatus tsdb_wal_close(TsdbWal *wal)
{
    if (wal == NULL) {
        return TSDB_ERR_ARGUMENT;
    }
    BOOL closed = CloseHandle(wal->file);
    free(wal);
    return closed ? TSDB_OK : TSDB_ERR_IO;
}
