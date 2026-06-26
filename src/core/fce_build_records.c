#include "../internal/fce_internal.h"

int cmp_build_by_key(const void *a, const void *b) {
    const BuildRecord *ra = (const BuildRecord *)a;
    const BuildRecord *rb = (const BuildRecord *)b;
    size_t n = ra->key_len < rb->key_len ? ra->key_len : rb->key_len;
    int c = n ? memcmp(ra->key, rb->key, n) : 0;
    if (c) return c;
    if (ra->key_len != rb->key_len) return ra->key_len < rb->key_len ? -1 : 1;
    return ra->order < rb->order ? -1 : (ra->order > rb->order);
}

int cmp_sorted_entry(const void *a, const void *b) {
    const SortedEntry *x = (const SortedEntry *)a;
    const SortedEntry *y = (const SortedEntry *)b;
    if (x->hash_hi != y->hash_hi) return x->hash_hi < y->hash_hi ? -1 : 1;
    if (x->hash_lo != y->hash_lo) return x->hash_lo < y->hash_lo ? -1 : 1;
    if (x->key_len != y->key_len) return x->key_len < y->key_len ? -1 : 1;
    if (x->key_offset != y->key_offset) return x->key_offset < y->key_offset ? -1 : 1;
    return 0;
}

int cmp_log_index_entry(const void *a, const void *b) {
    const LogIndexEntry *x = (const LogIndexEntry *)a;
    const LogIndexEntry *y = (const LogIndexEntry *)b;
    if (x->hash_hi != y->hash_hi) return x->hash_hi < y->hash_hi ? -1 : 1;
    if (x->hash_lo != y->hash_lo) return x->hash_lo < y->hash_lo ? -1 : 1;
    if (x->key_len != y->key_len) return x->key_len < y->key_len ? -1 : 1;
    if (x->key_offset != y->key_offset) return x->key_offset < y->key_offset ? -1 : 1;
    return x->order < y->order ? -1 : (x->order > y->order);
}

FceStatus dedupe_records(FceBuilder *b, BuildRecord **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!b->count) return FCE_OK;
    BuildRecord *tmp = (BuildRecord *)fce_xmalloc(b->count * sizeof(BuildRecord));
    if (!tmp) return FCE_ERR_OUT_OF_MEMORY;
    memcpy(tmp, b->records, b->count * sizeof(BuildRecord));
    qsort(tmp, b->count, sizeof(BuildRecord), cmp_build_by_key);
    BuildRecord *dedup = (BuildRecord *)fce_xmalloc(b->count * sizeof(BuildRecord));
    if (!dedup) {
        fce_free(tmp);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;
    for (size_t i = 0; i < b->count;) {
        size_t j = i + 1;
        BuildRecord latest = tmp[i];
        while (j < b->count && tmp[j].key_len == tmp[i].key_len && memcmp(tmp[j].key, tmp[i].key, tmp[i].key_len) == 0) {
            if (!b->schema.allow_duplicate_put) {
                fce_free(tmp);
                fce_free(dedup);
                return FCE_ERR_INVALID_ARGUMENT;
            }
            if (tmp[j].order > latest.order) latest = tmp[j];
            j++;
        }
        dedup[n++] = latest;
        i = j;
    }
    fce_free(tmp);
    *out = dedup;
    *out_count = n;
    return FCE_OK;
}

FceStatus write_keys_values(FceBuilder *b, BuildRecord *records, size_t count, SortedEntry *entries, uint64_t *keys_size, uint64_t *values_size) {
    char *kp = join_path_arena(b->arena, b->cache_dir, FCE_KEYS_FILE);
    char *vp = join_path_arena(b->arena, b->cache_dir, FCE_VALUES_FILE);
    if (!kp || !vp) return FCE_ERR_OUT_OF_MEMORY;
    FILE *kf = fopen(kp, "wb");
    FILE *vf = fopen(vp, "wb");
    if (!kf || !vf) {
        if (kf) fclose(kf);
        if (vf) fclose(vf);
        return FCE_ERR_IO;
    }
    uint64_t ko = 0, vo = 0;
    for (size_t i = 0; i < count; i++) {
        if (records[i].key_len > UINT32_MAX || records[i].value_len > UINT32_MAX) {
            fclose(kf);
            fclose(vf);
            return FCE_ERR_INVALID_ARGUMENT;
        }
        Hash128 h = hash_key(&b->schema, records[i].key, records[i].key_len);
        entries[i].hash_lo = h.lo;
        entries[i].hash_hi = h.hi;
        entries[i].key_offset = ko;
        entries[i].key_len = (uint32_t)records[i].key_len;
        entries[i].reserved0 = 0;
        entries[i].value_offset = vo;
        entries[i].value_len = (uint32_t)records[i].value_len;
        entries[i].flags = 0;
        if (records[i].key_len && fwrite(records[i].key, 1, records[i].key_len, kf) != records[i].key_len) {
            fclose(kf);
            fclose(vf);
            return FCE_ERR_IO;
        }
        if (records[i].value_len && fwrite(records[i].value, 1, records[i].value_len, vf) != records[i].value_len) {
            fclose(kf);
            fclose(vf);
            return FCE_ERR_IO;
        }
        ko += records[i].key_len;
        vo += records[i].value_len;
    }
    if (fclose(kf) != 0 || fclose(vf) != 0) return FCE_ERR_IO;
    *keys_size = ko;
    *values_size = vo;
    return FCE_OK;
}

