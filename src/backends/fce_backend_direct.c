#include "../internal/fce_internal.h"

FceStatus freeze_direct(FceBuilder *b) {
    BuildRecord *records = NULL;
    size_t count = 0;
    FceStatus st = dedupe_records(b, &records, &count);
    if (st != FCE_OK) return st;
    uint64_t max_key = 0;
    for (size_t i = 0; i < count; i++) {
        if (records[i].key_len != 8) {
            fce_free(records);
            return FCE_ERR_INVALID_ARGUMENT;
        }
        uint64_t k = rd64(records[i].key);
        if (k > max_key) max_key = k;
    }
    if (count && max_key + 1 > count * 4 && !(b->schema.user_flags & FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE)) {
        fce_free(records);
        return FCE_ERR_INVALID_ARGUMENT;
    }
    size_t slots_count = (size_t)(max_key + 1);
    if (max_key > (uint64_t)(SIZE_MAX / sizeof(DirectSlot) - 1)) {
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    DirectSlot *slots = (DirectSlot *)fce_xcalloc(slots_count ? slots_count : 1, sizeof(*slots));
    SortedEntry *entries = (SortedEntry *)fce_xcalloc(count ? count : 1, sizeof(*entries));
    if (!slots || !entries) {
        fce_free(slots);
        fce_free(entries);
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    uint64_t keys_size = 0, values_size = 0;
    st = write_keys_values(b, records, count, entries, &keys_size, &values_size);
    if (st == FCE_OK) {
        for (size_t i = 0; i < count; i++) {
            uint64_t k = rd64(records[i].key);
            slots[k].present = 1;
            slots[k].key_u64 = k;
            slots[k].key_offset = entries[i].key_offset;
            slots[k].key_len = entries[i].key_len;
            slots[k].value_offset = entries[i].value_offset;
            slots[k].value_len = entries[i].value_len;
        }
        char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
        st = ip ? write_file(ip, slots, slots_count * sizeof(*slots)) : FCE_ERR_OUT_OF_MEMORY;
    }
    if (st == FCE_OK) {
        FceManifestInfo m = make_manifest(&b->schema, (uint64_t)count);
        m.index_size = slots_count * sizeof(*slots);
        m.keys_size = keys_size;
        m.values_size = values_size;
        m.backend_meta0 = slots_count;
        char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
        char *kp = join_path_arena(b->arena, b->cache_dir, FCE_KEYS_FILE);
        char *vp = join_path_arena(b->arena, b->cache_dir, FCE_VALUES_FILE);
        m.index_crc64 = crc64_file(ip);
        m.keys_crc64 = crc64_file(kp);
        m.values_crc64 = crc64_file(vp);
        st = write_manifest(b->cache_dir, &m);
    }
    fce_free(slots);
    fce_free(entries);
    fce_free(records);
    return st;
}

FceStatus get_direct(FceReader *r, uint64_t key, const void **out_value, size_t *out_value_len) {
    size_t n = r->index_blob.size / sizeof(DirectSlot);
    if (key >= n) return FCE_ERR_NOT_FOUND;
    DirectSlot *s = (DirectSlot *)r->index_blob.data;
    DirectSlot *slot = &s[key];
    if (!slot->present || slot->key_u64 != key) return FCE_ERR_NOT_FOUND;
    if (!range_ok(slot->value_offset, slot->value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
    *out_value = r->values_blob.data + slot->value_offset;
    *out_value_len = slot->value_len;
    return FCE_OK;
}

