#include "../internal/fce_internal.h"

FceStatus freeze_log(FceBuilder *b) {
    char *lp = join_path_arena(b->arena, b->cache_dir, FCE_LOG_FILE);
    if (!lp) return FCE_ERR_OUT_OF_MEMORY;
    FILE *f = fopen(lp, "wb");
    if (!f) return FCE_ERR_IO;
    uint64_t log_size = 0;
    for (size_t i = 0; i < b->count; i++) {
        uint64_t head[3] = {FCE_LOG_MAGIC, (uint64_t)b->records[i].key_len, (uint64_t)b->records[i].value_len};
        uint64_t crc = 0;
        crc = crc64_update(crc, head, sizeof(head));
        crc = crc64_update(crc, b->records[i].key, b->records[i].key_len);
        crc = crc64_update(crc, b->records[i].value, b->records[i].value_len);
        if (fwrite(head, 1, sizeof(head), f) != sizeof(head) ||
            (b->records[i].key_len && fwrite(b->records[i].key, 1, b->records[i].key_len, f) != b->records[i].key_len) ||
            (b->records[i].value_len && fwrite(b->records[i].value, 1, b->records[i].value_len, f) != b->records[i].value_len) ||
            fwrite(&crc, 1, sizeof(crc), f) != sizeof(crc)) {
            fclose(f);
            return FCE_ERR_IO;
        }
        log_size += sizeof(head) + b->records[i].key_len + b->records[i].value_len + sizeof(crc);
    }
    if (fclose(f) != 0) return FCE_ERR_IO;
    FceManifestInfo m = make_manifest(&b->schema, (uint64_t)b->count);
    m.log_size = log_size;
    m.log_crc64 = crc64_file(lp);
    return write_manifest(b->cache_dir, &m);
}

FceStatus build_log_index(FceReader *r) {
    if (r->schema.backend != FCE_BACKEND_LOG) return FCE_OK;
    if (r->manifest.record_count > SIZE_MAX / sizeof(LogIndexEntry)) return FCE_ERR_OUT_OF_MEMORY;
    r->log_index = (LogIndexEntry *)fce_arena_alloc(r->arena, (r->manifest.record_count ? (size_t)r->manifest.record_count : 1) * sizeof(LogIndexEntry), sizeof(LogIndexEntry));
    if (!r->log_index) return FCE_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    size_t order = 0;
    while (pos + 32 <= r->log_blob.size) {
        size_t rec_start = pos;
        uint64_t magic = rd64(r->log_blob.data + pos);
        uint64_t kl = rd64(r->log_blob.data + pos + 8);
        uint64_t vl = rd64(r->log_blob.data + pos + 16);
        if (magic != FCE_LOG_MAGIC || kl > UINT32_MAX || vl > UINT32_MAX || kl > SIZE_MAX || vl > SIZE_MAX) return FCE_ERR_CORRUPT;
        pos += 24;
        if (!range_ok(pos, kl, r->log_blob.size)) return FCE_ERR_CORRUPT;
        uint64_t key_offset = pos;
        uint8_t *kp = r->log_blob.data + pos;
        pos += (size_t)kl;
        if (!range_ok(pos, vl, r->log_blob.size)) return FCE_ERR_CORRUPT;
        uint64_t value_offset = pos;
        pos += (size_t)vl;
        if (!range_ok(pos, 8, r->log_blob.size)) return FCE_ERR_CORRUPT;
        uint64_t stored_crc = rd64(r->log_blob.data + pos);
        uint64_t crc = crc64_update(0, r->log_blob.data + rec_start, 24 + (size_t)kl + (size_t)vl);
        if (crc != stored_crc) return FCE_ERR_CORRUPT;
        pos += 8;
        if (order >= r->manifest.record_count) return FCE_ERR_CORRUPT;
        Hash128 h = hash_key(&r->schema, kp, (size_t)kl);
        r->log_index[order].hash_lo = h.lo;
        r->log_index[order].hash_hi = h.hi;
        r->log_index[order].key_offset = key_offset;
        r->log_index[order].key_len = (uint32_t)kl;
        r->log_index[order].value_offset = value_offset;
        r->log_index[order].value_len = (uint32_t)vl;
        r->log_index[order].order = (uint64_t)order;
        order++;
    }
    if (pos != r->log_blob.size || order != r->manifest.record_count) return FCE_ERR_CORRUPT;
    r->log_index_count = order;
    qsort(r->log_index, r->log_index_count, sizeof(LogIndexEntry), cmp_log_index_entry);
    return FCE_OK;
}

FceStatus get_log(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    Hash128 h = hash_key(&r->schema, key, key_len);
    size_t lo = 0, hi = r->log_index_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        LogIndexEntry *e = &r->log_index[mid];
        if (e->hash_hi < h.hi || (e->hash_hi == h.hi && e->hash_lo < h.lo)) lo = mid + 1;
        else hi = mid;
    }
    LogIndexEntry *best = NULL;
    for (size_t i = lo; i < r->log_index_count && r->log_index[i].hash_hi == h.hi && r->log_index[i].hash_lo == h.lo; i++) {
        LogIndexEntry *e = &r->log_index[i];
        if (key_equal(r->log_blob.data, r->log_blob.size, e->key_offset, e->key_len, key, key_len)) {
            if (!best || e->order > best->order) best = e;
        }
    }
    if (!best) return FCE_ERR_NOT_FOUND;
    if (!range_ok(best->value_offset, best->value_len, r->log_blob.size)) return FCE_ERR_CORRUPT;
    *out_value = r->log_blob.data + best->value_offset;
    *out_value_len = best->value_len;
    return FCE_OK;
}

FceStatus fce_log_append(const char *cache_dir, const void *key, size_t key_len, const void *value, size_t value_len) {
    if (!cache_dir || (!key && key_len) || (!value && value_len)) return FCE_ERR_INVALID_ARGUMENT;
    FceFileLock lock;
    FceStatus st = fce_cache_lock_acquire(cache_dir, &lock);
    if (st != FCE_OK) return st;
    FceManifestInfo m;
    st = read_manifest(cache_dir, &m);
    if (st != FCE_OK) {
        fce_cache_lock_release(&lock);
        return st;
    }
    if (m.backend_kind != FCE_BACKEND_LOG) {
        fce_cache_lock_release(&lock);
        return FCE_ERR_INVALID_ARGUMENT;
    }
    FceSchema s = schema_from_manifest(&m);
    void *encoded_key = NULL;
    void *encoded_value = NULL;
    size_t encoded_key_len = 0;
    size_t encoded_value_len = 0;
    st = fce_codec_encode(s.key_codec, key, key_len, &encoded_key, &encoded_key_len);
    if (st != FCE_OK) {
        fce_cache_lock_release(&lock);
        return st;
    }
    st = fce_codec_encode(s.value_codec, value, value_len, &encoded_value, &encoded_value_len);
    if (st != FCE_OK) {
        fce_free(encoded_key);
        fce_cache_lock_release(&lock);
        return st;
    }
    char *lp = join_path_heap(cache_dir, FCE_LOG_FILE);
    if (!lp) {
        fce_free(encoded_key);
        fce_free(encoded_value);
        fce_cache_lock_release(&lock);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    FILE *f = fopen(lp, "ab");
    if (!f) {
        fce_free(lp);
        fce_free(encoded_key);
        fce_free(encoded_value);
        fce_cache_lock_release(&lock);
        return FCE_ERR_IO;
    }
    uint64_t head[3] = {FCE_LOG_MAGIC, (uint64_t)encoded_key_len, (uint64_t)encoded_value_len};
    uint64_t crc = 0;
    crc = crc64_update(crc, head, sizeof(head));
    crc = crc64_update(crc, encoded_key, encoded_key_len);
    crc = crc64_update(crc, encoded_value, encoded_value_len);
    if (fwrite(head, 1, sizeof(head), f) != sizeof(head) ||
        (encoded_key_len && fwrite(encoded_key, 1, encoded_key_len, f) != encoded_key_len) ||
        (encoded_value_len && fwrite(encoded_value, 1, encoded_value_len, f) != encoded_value_len) ||
        fwrite(&crc, 1, sizeof(crc), f) != sizeof(crc)) {
        fclose(f);
        fce_free(lp);
        fce_free(encoded_key);
        fce_free(encoded_value);
        fce_cache_lock_release(&lock);
        return FCE_ERR_IO;
    }
    if (fclose(f) != 0) {
        fce_free(lp);
        fce_free(encoded_key);
        fce_free(encoded_value);
        fce_cache_lock_release(&lock);
        return FCE_ERR_IO;
    }
    m.record_count++;
    m.log_size += sizeof(head) + encoded_key_len + encoded_value_len + sizeof(crc);
    m.log_crc64 = crc64_file(lp);
    fce_free(lp);
    fce_free(encoded_key);
    fce_free(encoded_value);
    st = write_manifest(cache_dir, &m);
    fce_cache_lock_release(&lock);
    return st;
}
