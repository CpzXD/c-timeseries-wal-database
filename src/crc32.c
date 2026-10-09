#include "tsdb/crc32.h"

uint32_t tsdb_crc32_start(void)
{
    return 0xFFFFFFFFu;
}

uint32_t tsdb_crc32_finish(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t tsdb_crc32_update(uint32_t state, const uint8_t *bytes, size_t length)
{
    for (size_t pos = 0; pos < length; pos++) {
        state ^= bytes[pos];

        for (int bit = 0; bit < 8; bit++) {
            if (state & 1u) {
                state = (state >> 1) ^ 0xEDB88320u;
            } else {
                state >>= 1;
            }
        }
    }
    return state;
}