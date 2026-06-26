#include "../internal/fce_internal.h"

uint64_t crc64_update(uint64_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint64_t mask = 0ULL - (crc & 1ULL);
            crc = (crc >> 1) ^ (0xC96C5795D7870F42ULL & mask);
        }
    }
    return ~crc;
}

uint64_t crc64_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[8192];
    uint64_t crc = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) crc = crc64_update(crc, buf, n);
    fclose(f);
    return crc;
}

