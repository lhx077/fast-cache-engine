#include "../internal/fce_internal.h"

static FceStatus builder_grow(FceBuilder *b) {
    if (b->count < b->cap) return FCE_OK;
    size_t nc = b->cap ? b->cap * 2 : 64;
    if (nc < b->cap || nc > SIZE_MAX / sizeof(BuildRecord)) return FCE_ERR_OUT_OF_MEMORY;
    BuildRecord *nr = (BuildRecord *)fce_xrealloc(b->records, nc * sizeof(BuildRecord));
    if (!nr) return FCE_ERR_OUT_OF_MEMORY;
    b->records = nr;
    b->cap = nc;
    return FCE_OK;
}

FceStatus fce_builder_open(const char *cache_dir, const FceSchema *schema, FceBuilder **out_builder) {
    if (!cache_dir || !schema || !out_builder) return FCE_ERR_INVALID_ARGUMENT;
    *out_builder = NULL;
    FceStatus st = validate_schema(schema);
    if (st != FCE_OK) return st;
    st = ensure_dir(cache_dir);
    if (st != FCE_OK) return st;
    FceBuilder *b = (FceBuilder *)fce_xcalloc(1, sizeof(*b));
    if (!b) return FCE_ERR_OUT_OF_MEMORY;
    st = fce_arena_create(&b->arena);
    if (st != FCE_OK) {
        fce_free(b);
        return st;
    }
    b->cache_dir = (char *)fce_arena_memdup(b->arena, cache_dir, strlen(cache_dir) + 1);
    if (!b->cache_dir) {
        fce_arena_destroy(b->arena);
        fce_free(b);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    b->schema = *schema;
    *out_builder = b;
    return FCE_OK;
}

FceStatus fce_builder_put(FceBuilder *b, const void *key, size_t key_len, const void *value, size_t value_len) {
    if (!b || (!key && key_len) || (!value && value_len) || b->frozen) return FCE_ERR_INVALID_ARGUMENT;
    if (b->schema.fixed_key_size && key_len != b->schema.fixed_key_size) return FCE_ERR_INVALID_ARGUMENT;
    if (b->schema.fixed_value_size && value_len != b->schema.fixed_value_size) return FCE_ERR_INVALID_ARGUMENT;
    FceStatus st = builder_grow(b);
    if (st != FCE_OK) return st;
    void *encoded_key = NULL;
    void *encoded_value = NULL;
    size_t encoded_key_len = 0;
    size_t encoded_value_len = 0;
    st = fce_codec_encode(b->schema.key_codec, key, key_len, &encoded_key, &encoded_key_len);
    if (st != FCE_OK) return st;
    st = fce_codec_encode(b->schema.value_codec, value, value_len, &encoded_value, &encoded_value_len);
    if (st != FCE_OK) {
        fce_free(encoded_key);
        return st;
    }
    BuildRecord *r = &b->records[b->count];
    memset(r, 0, sizeof(*r));
    r->key = (uint8_t *)fce_arena_memdup(b->arena, encoded_key, encoded_key_len);
    r->value = (uint8_t *)fce_arena_memdup(b->arena, encoded_value, encoded_value_len);
    fce_free(encoded_key);
    fce_free(encoded_value);
    if (!r->key || !r->value) return FCE_ERR_OUT_OF_MEMORY;
    r->key_len = encoded_key_len;
    r->value_len = encoded_value_len;
    r->order = (uint64_t)b->count;
    b->count++;
    return FCE_OK;
}

FceStatus fce_builder_put_u64(FceBuilder *builder, uint64_t key, const void *value, size_t value_len) {
    uint8_t buf[8];
    wr64(buf, key);
    return fce_builder_put(builder, buf, sizeof(buf), value, value_len);
}

FceStatus fce_builder_put_u128(FceBuilder *builder, uint64_t key_lo, uint64_t key_hi, const void *value, size_t value_len) {
    uint8_t buf[16];
    wr64(buf, key_lo);
    wr64(buf + 8, key_hi);
    return fce_builder_put(builder, buf, sizeof(buf), value, value_len);
}

FceStatus fce_builder_bulk_put(FceBuilder *builder, const void *const *keys, const size_t *key_lens, const void *const *values, const size_t *value_lens, size_t count) {
    if (!builder || !keys || !key_lens || !values || !value_lens) return FCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++) {
        FceStatus st = fce_builder_put(builder, keys[i], key_lens[i], values[i], value_lens[i]);
        if (st != FCE_OK) return st;
    }
    return FCE_OK;
}

FceStatus fce_builder_freeze(FceBuilder *b) {
    if (!b || b->frozen) return FCE_ERR_INVALID_ARGUMENT;
    FceFileLock lock;
    FceStatus st = fce_cache_lock_acquire(b->cache_dir, &lock);
    if (st != FCE_OK) return st;
    switch (b->schema.backend) {
        case FCE_BACKEND_SORTED_INDEX:
            st = freeze_sorted_like(b, b->schema.backend);
            break;
        case FCE_BACKEND_RADIX:
            st = freeze_radix(b);
            break;
        case FCE_BACKEND_DIRECT_TABLE:
            st = freeze_direct(b);
            break;
        case FCE_BACKEND_MPH:
            st = freeze_mph(b);
            break;
        case FCE_BACKEND_LOG:
            st = freeze_log(b);
            break;
        default:
            st = FCE_ERR_UNSUPPORTED;
            break;
    }
    fce_cache_lock_release(&lock);
    if (st == FCE_OK) b->frozen = 1;
    return st;
}

FceStatus fce_builder_close(FceBuilder *builder) {
    if (!builder) return FCE_OK;
    fce_free(builder->records);
    fce_arena_destroy(builder->arena);
    fce_free(builder);
    return FCE_OK;
}
