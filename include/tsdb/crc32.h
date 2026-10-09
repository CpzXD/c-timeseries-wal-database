#ifndef TSDB_CRC32_H
#define TSDB_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t tsdb_crc32_start(void);
uint32_t tsdb_crc32_update(uint32_t state, const uint8_t *bytes, size_t length);
uint32_t tsdb_crc32_finish(uint32_t state);

#endif