#include "../internal/fce_internal.h"

uint64_t rd64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

void wr64(void *p, uint64_t v) {
    memcpy(p, &v, sizeof(v));
}

uint64_t fnv1a64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

Hash128 hash_key(const FceSchema *schema, const void *data, size_t len) {
    Hash128 h;
    if (schema && (schema->user_flags & FCE_FLAG_FORCE_HASH_COLLISION)) {
        h.lo = 0x1111111111111111ULL;
        h.hi = 0x2222222222222222ULL;
        return h;
    }
    h.lo = fnv1a64(data, len, 0xcbf29ce484222325ULL);
    h.hi = fnv1a64(data, len, 0x9e3779b97f4a7c15ULL);
    return h;
}

int range_ok(uint64_t off, uint64_t len, uint64_t size) {
    return off <= size && len <= size - off;
}

int key_equal(const uint8_t *base, size_t size, uint64_t off, uint32_t len, const void *key, size_t key_len) {
    return key_len == len && range_ok(off, len, size) && (len == 0 || memcmp(base + off, key, len) == 0);
}

int key_bytes_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c) return c;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

