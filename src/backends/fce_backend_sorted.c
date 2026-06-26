#include "../internal/fce_internal.h"

FceStatus freeze_sorted_like(FceBuilder *b, FceBackendKind physical_backend) {
    BuildRecord *records = NULL;
    size_t count = 0;
    FceStatus st = dedupe_records(b, &records, &count);
    if (st != FCE_OK) return st;
    SortedEntry *entries = NULL;
    if (count) {
        entries = (SortedEntry *)fce_xcalloc(count, sizeof(*entries));
        if (!entries) {
            fce_free(records);
            return FCE_ERR_OUT_OF_MEMORY;
        }
    }
    uint64_t keys_size = 0, values_size = 0;
    st = write_keys_values(b, records, count, entries, &keys_size, &values_size);
    if (st == FCE_OK) {
        if (physical_backend == FCE_BACKEND_RADIX) {
            qsort(records, count, sizeof(BuildRecord), cmp_build_by_key);
        } else {
            qsort(entries, count, sizeof(SortedEntry), cmp_sorted_entry);
        }
        char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
        if (!ip) st = FCE_ERR_OUT_OF_MEMORY;
        else st = write_file(ip, entries, count * sizeof(SortedEntry));
    }
    if (st == FCE_OK) {
        FceManifestInfo m = make_manifest(&b->schema, (uint64_t)count);
        m.index_size = count * sizeof(SortedEntry);
        m.keys_size = keys_size;
        m.values_size = values_size;
        char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
        char *kp = join_path_arena(b->arena, b->cache_dir, FCE_KEYS_FILE);
        char *vp = join_path_arena(b->arena, b->cache_dir, FCE_VALUES_FILE);
        if (!ip || !kp || !vp) st = FCE_ERR_OUT_OF_MEMORY;
        else {
            m.index_crc64 = crc64_file(ip);
            m.keys_crc64 = crc64_file(kp);
            m.values_crc64 = crc64_file(vp);
            st = write_manifest(b->cache_dir, &m);
        }
    }
    fce_free(entries);
    fce_free(records);
    return st;
}

FceStatus get_sorted(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    SortedEntry *e = (SortedEntry *)r->index_blob.data;
    size_t n = r->index_blob.size / sizeof(SortedEntry);
    Hash128 h = hash_key(&r->schema, key, key_len);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e[mid].hash_hi < h.hi || (e[mid].hash_hi == h.hi && e[mid].hash_lo < h.lo)) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo; i < n && e[i].hash_hi == h.hi && e[i].hash_lo == h.lo; i++) {
        if (key_equal(r->keys_blob.data, r->keys_blob.size, e[i].key_offset, e[i].key_len, key, key_len)) {
            if (!range_ok(e[i].value_offset, e[i].value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
            *out_value = r->values_blob.data + e[i].value_offset;
            *out_value_len = e[i].value_len;
            return FCE_OK;
        }
    }
    return FCE_ERR_NOT_FOUND;
}

