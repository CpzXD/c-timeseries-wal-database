#ifndef TSDB_INTERNAL_BINARY_H
#define TSDB_INTERNAL_BINARY_H

#include <stddef.h>
#include <stdint.h>

static inline void tsdb_put_u32_le(uint8_t *out, uint32_t value)
{
    for (size_t byte = 0u; byte < sizeof value; ++byte) {
        out[byte] = (uint8_t)(value >> (8u * byte));
    }
}

static inline uint32_t tsdb_get_u32_le(const uint8_t *in)
{
    uint32_t value = 0u;
    for (size_t byte = 0u; byte < sizeof value; ++byte) {
        value |= (uint32_t)in[byte] << (8u * byte);
    }
    return value;
}

static inline void tsdb_put_u64_le(uint8_t *out, uint64_t value)
{
    for (size_t byte = 0u; byte < sizeof value; ++byte) {
        out[byte] = (uint8_t)(value >> (8u * byte));
    }
}

static inline uint64_t tsdb_get_u64_le(const uint8_t *in)
{
    uint64_t value = 0u;
    for (size_t byte = 0u; byte < sizeof value; ++byte) {
        value |= (uint64_t)in[byte] << (8u * byte);
    }
    return value;
}

#endif
