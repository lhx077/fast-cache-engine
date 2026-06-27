#include "../internal/fce_internal.h"

FceStatus validate_reader_files(FceReader *r, const FceSchema *expected) {
    FceManifestInfo *m = &r->manifest;
    if (expected && fce_schema_hash(expected) != m->schema_hash) return FCE_ERR_SCHEMA_MISMATCH;
    if (m->index_size != r->index_blob.size || m->keys_size != r->keys_blob.size ||
        m->values_size != r->values_blob.size || m->log_size != r->log_blob.size) return FCE_ERR_CORRUPT;
    char *ip = join_path_arena(r->arena, r->cache_dir, FCE_INDEX_FILE);
    char *kp = join_path_arena(r->arena, r->cache_dir, FCE_KEYS_FILE);
    char *vp = join_path_arena(r->arena, r->cache_dir, FCE_VALUES_FILE);
    char *lp = join_path_arena(r->arena, r->cache_dir, FCE_LOG_FILE);
    if (!ip || !kp || !vp || !lp) return FCE_ERR_OUT_OF_MEMORY;
    if (m->index_size && crc64_file(ip) != m->index_crc64) return FCE_ERR_CORRUPT;
    if (m->keys_size && crc64_file(kp) != m->keys_crc64) return FCE_ERR_CORRUPT;
    if (m->values_size && crc64_file(vp) != m->values_crc64) return FCE_ERR_CORRUPT;
    if (m->log_size && crc64_file(lp) != m->log_crc64) return FCE_ERR_CORRUPT;
    if (m->backend_kind == FCE_BACKEND_SORTED_INDEX) {
        if (m->index_size % sizeof(SortedEntry) != 0) return FCE_ERR_CORRUPT;
        SortedEntry *e = (SortedEntry *)r->index_blob.data;
        size_t n = r->index_blob.size / sizeof(SortedEntry);
        if (n != m->record_count) return FCE_ERR_CORRUPT;
        for (size_t i = 0; i < n; i++) {
            if (!range_ok(e[i].key_offset, e[i].key_len, r->keys_blob.size) ||
                !range_ok(e[i].value_offset, e[i].value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
        }
    } else if (m->backend_kind == FCE_BACKEND_RADIX) {
        if (r->index_blob.size < sizeof(RadixHeader)) return FCE_ERR_CORRUPT;
        RadixHeader *h = (RadixHeader *)r->index_blob.data;
        if (h->magic != FCE_RADIX_MAGIC || h->node_count == 0 || h->value_count != m->record_count) return FCE_ERR_CORRUPT;
        if (!range_ok(h->nodes_offset, h->node_count * sizeof(RadixNode), r->index_blob.size) ||
            !range_ok(h->edges_offset, h->edge_count * sizeof(RadixEdge), r->index_blob.size) ||
            !range_ok(h->values_offset, h->value_count * sizeof(RadixValue), r->index_blob.size)) return FCE_ERR_CORRUPT;
        if (h->nodes_offset != sizeof(RadixHeader) ||
            h->edges_offset != h->nodes_offset + h->node_count * sizeof(RadixNode) ||
            h->values_offset != h->edges_offset + h->edge_count * sizeof(RadixEdge) ||
            h->values_offset + h->value_count * sizeof(RadixValue) != r->index_blob.size) return FCE_ERR_CORRUPT;
        RadixNode *nodes = (RadixNode *)(r->index_blob.data + h->nodes_offset);
        RadixEdge *edges = (RadixEdge *)(r->index_blob.data + h->edges_offset);
        RadixValue *values = (RadixValue *)(r->index_blob.data + h->values_offset);
        size_t terminals = 0;
        for (size_t i = 0; i < h->node_count; i++) {
            if ((uint64_t)nodes[i].first_edge + nodes[i].edge_count > h->edge_count) return FCE_ERR_CORRUPT;
            if (nodes[i].value_index != FCE_RADIX_NO_VALUE) {
                if (nodes[i].value_index >= h->value_count) return FCE_ERR_CORRUPT;
                terminals++;
            }
            uint8_t prev = 0;
            for (uint32_t j = 0; j < nodes[i].edge_count; j++) {
                RadixEdge *e = &edges[nodes[i].first_edge + j];
                if (e->child_index >= h->node_count) return FCE_ERR_CORRUPT;
                if (j && e->label <= prev) return FCE_ERR_CORRUPT;
                prev = e->label;
            }
        }
        if (terminals != m->record_count) return FCE_ERR_CORRUPT;
        for (size_t i = 0; i < h->value_count; i++) {
            if (!range_ok(values[i].key_offset, values[i].key_len, r->keys_blob.size) ||
                !range_ok(values[i].value_offset, values[i].value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
        }
    } else if (m->backend_kind == FCE_BACKEND_DIRECT_TABLE) {
        if (m->index_size % sizeof(DirectSlot) != 0) return FCE_ERR_CORRUPT;
        DirectSlot *s = (DirectSlot *)r->index_blob.data;
        size_t present = 0, n = r->index_blob.size / sizeof(DirectSlot);
        for (size_t i = 0; i < n; i++) if (s[i].present) {
            present++;
            if (!range_ok(s[i].key_offset, s[i].key_len, r->keys_blob.size) ||
                !range_ok(s[i].value_offset, s[i].value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
        }
        if (present != m->record_count) return FCE_ERR_CORRUPT;
    } else if (m->backend_kind == FCE_BACKEND_MPH) {
        if (r->index_blob.size < sizeof(MphHeader)) return FCE_ERR_CORRUPT;
        MphHeader *h = (MphHeader *)r->index_blob.data;
        if (h->magic != FCE_MPH_MAGIC || h->bucket_count != m->record_count || h->slot_count != m->record_count) return FCE_ERR_CORRUPT;
        if (!range_ok(h->buckets_offset, h->bucket_count * sizeof(MphBucket), r->index_blob.size) ||
            !range_ok(h->slots_offset, h->slot_count * sizeof(MphSlot), r->index_blob.size)) return FCE_ERR_CORRUPT;
        if (h->buckets_offset != sizeof(MphHeader) ||
            h->slots_offset != h->buckets_offset + h->bucket_count * sizeof(MphBucket) ||
            h->slots_offset + h->slot_count * sizeof(MphSlot) != r->index_blob.size) return FCE_ERR_CORRUPT;
        MphSlot *s = (MphSlot *)(r->index_blob.data + h->slots_offset);
        size_t present = 0, n = (size_t)h->slot_count;
        for (size_t i = 0; i < n; i++) if (s[i].present) {
            present++;
            if (!range_ok(s[i].key_offset, s[i].key_len, r->keys_blob.size) ||
                !range_ok(s[i].value_offset, s[i].value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
        }
        if (present != m->record_count) return FCE_ERR_CORRUPT;
    }
    return FCE_OK;
}

FceStatus fce_reader_open_expected(const char *cache_dir, const FceSchema *expected_schema, FceReader **out_reader) {
    if (!cache_dir || !out_reader) return FCE_ERR_INVALID_ARGUMENT;
    *out_reader = NULL;
    FceReader *r = (FceReader *)fce_xcalloc(1, sizeof(*r));
    if (!r) return FCE_ERR_OUT_OF_MEMORY;
    FceStatus st = fce_arena_create(&r->arena);
    if (st != FCE_OK) {
        fce_free(r);
        return st;
    }
    r->cache_dir = (char *)fce_arena_memdup(r->arena, cache_dir, strlen(cache_dir) + 1);
    if (!r->cache_dir) st = FCE_ERR_OUT_OF_MEMORY;
    if (st == FCE_OK) {
        st = fce_cache_lock_acquire_shared(cache_dir, &r->cache_lock);
        if (st == FCE_OK) r->has_cache_lock = 1;
    }
    if (st == FCE_OK) st = read_manifest(cache_dir, &r->manifest);
    if (st == FCE_OK) {
        r->schema = schema_from_manifest(&r->manifest);
        char *ip = join_path_arena(r->arena, cache_dir, FCE_INDEX_FILE);
        char *kp = join_path_arena(r->arena, cache_dir, FCE_KEYS_FILE);
        char *vp = join_path_arena(r->arena, cache_dir, FCE_VALUES_FILE);
        char *lp = join_path_arena(r->arena, cache_dir, FCE_LOG_FILE);
        if (!ip || !kp || !vp || !lp) st = FCE_ERR_OUT_OF_MEMORY;
        if (st == FCE_OK) st = read_file_arena(r->arena, ip, &r->index_blob, r->schema.mmap_read);
        if (st == FCE_OK) st = read_file_arena(r->arena, kp, &r->keys_blob, r->schema.mmap_read);
        if (st == FCE_OK) st = read_file_arena(r->arena, vp, &r->values_blob, r->schema.mmap_read);
        if (st == FCE_OK) st = read_file_arena(r->arena, lp, &r->log_blob, r->schema.mmap_read);
    }
    if (st == FCE_OK) st = validate_reader_files(r, expected_schema);
    if (st == FCE_OK) st = build_log_index(r);
    if (st != FCE_OK) {
        fce_reader_close(r);
        return st;
    }
    *out_reader = r;
    return FCE_OK;
}

FceStatus fce_reader_open(const char *cache_dir, FceReader **out_reader) {
    return fce_reader_open_expected(cache_dir, NULL, out_reader);
}

static FceStatus fce_reader_get_canonical(FceReader *reader, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    if (!reader || (!key && key_len) || !out_value || !out_value_len) return FCE_ERR_INVALID_ARGUMENT;
    *out_value = NULL;
    *out_value_len = 0;
    switch (reader->schema.backend) {
        case FCE_BACKEND_SORTED_INDEX:
            return get_sorted(reader, key, key_len, out_value, out_value_len);
        case FCE_BACKEND_RADIX:
            return get_radix(reader, key, key_len, out_value, out_value_len);
        case FCE_BACKEND_DIRECT_TABLE:
            if (key_len != 8) return FCE_ERR_INVALID_ARGUMENT;
            return get_direct(reader, rd64(key), out_value, out_value_len);
        case FCE_BACKEND_MPH:
            return get_mph(reader, key, key_len, out_value, out_value_len);
        case FCE_BACKEND_LOG:
            return get_log(reader, key, key_len, out_value, out_value_len);
        default:
            return FCE_ERR_UNSUPPORTED;
    }
}

FceStatus fce_reader_get(FceReader *reader, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    if (!reader || (!key && key_len) || !out_value || !out_value_len) return FCE_ERR_INVALID_ARGUMENT;
    void *encoded_key = NULL;
    const void *canonical_key = key;
    size_t canonical_key_len = key_len;
    FceStatus st = FCE_OK;
    if (reader->schema.key_codec != FCE_CODEC_NONE && reader->schema.key_codec != FCE_CODEC_USER_BYTES) {
        st = fce_codec_encode(reader->schema.key_codec, key, key_len, &encoded_key, &canonical_key_len);
        if (st != FCE_OK) return st;
        canonical_key = encoded_key;
    }
    const void *encoded_value = NULL;
    size_t encoded_value_len = 0;
    st = fce_reader_get_canonical(reader, canonical_key, canonical_key_len, &encoded_value, &encoded_value_len);
    fce_free(encoded_key);
    if (st != FCE_OK) return st;
    if (reader->schema.value_codec == FCE_CODEC_NONE || reader->schema.value_codec == FCE_CODEC_USER_BYTES) {
        *out_value = encoded_value;
        *out_value_len = encoded_value_len;
        return FCE_OK;
    }
    void *decoded = NULL;
    size_t decoded_len = 0;
    st = fce_codec_decode(reader->schema.value_codec, encoded_value, encoded_value_len, &decoded, &decoded_len);
    if (st != FCE_OK) return st;
    void *owned = fce_arena_memdup(reader->arena, decoded, decoded_len);
    fce_free(decoded);
    if (!owned) return FCE_ERR_OUT_OF_MEMORY;
    *out_value = owned;
    *out_value_len = decoded_len;
    return FCE_OK;
}

FceStatus fce_reader_get_u64(FceReader *reader, uint64_t key, const void **out_value, size_t *out_value_len) {
    uint8_t buf[8];
    wr64(buf, key);
    return fce_reader_get(reader, buf, sizeof(buf), out_value, out_value_len);
}

FceStatus fce_reader_get_u128(FceReader *reader, uint64_t key_lo, uint64_t key_hi, const void **out_value, size_t *out_value_len) {
    uint8_t buf[16];
    wr64(buf, key_lo);
    wr64(buf + 8, key_hi);
    return fce_reader_get(reader, buf, sizeof(buf), out_value, out_value_len);
}

FceStatus fce_reader_get_copy(FceReader *reader, const void *key, size_t key_len, void *out_value, size_t *inout_value_len) {
    if (!inout_value_len) return FCE_ERR_INVALID_ARGUMENT;
    const void *v = NULL;
    size_t n = 0;
    FceStatus st = fce_reader_get(reader, key, key_len, &v, &n);
    if (st != FCE_OK) return st;
    if (!out_value || *inout_value_len < n) {
        *inout_value_len = n;
        return FCE_ERR_INVALID_ARGUMENT;
    }
    memcpy(out_value, v, n);
    *inout_value_len = n;
    return FCE_OK;
}

FceStatus fce_reader_get_batch(FceReader *reader, const void *const *keys, const size_t *key_lens, const void **out_values, size_t *out_value_lens, FceStatus *out_statuses, size_t count) {
    if (!reader || !keys || !key_lens || !out_values || !out_value_lens || !out_statuses) return FCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++) {
        out_values[i] = NULL;
        out_value_lens[i] = 0;
        out_statuses[i] = fce_reader_get(reader, keys[i], key_lens[i], &out_values[i], &out_value_lens[i]);
    }
    return FCE_OK;
}

FceStatus fce_reader_prefix_scan(FceReader *reader, const void *prefix, size_t prefix_len, FceIterator **out_iterator) {
    if (!reader || (!prefix && prefix_len) || !out_iterator) return FCE_ERR_INVALID_ARGUMENT;
    *out_iterator = NULL;
    FceIterator *it = (FceIterator *)fce_xcalloc(1, sizeof(*it));
    if (!it) return FCE_ERR_OUT_OF_MEMORY;
    FceStatus st = fce_arena_create(&it->arena);
    if (st != FCE_OK) {
        fce_free(it);
        return st;
    }
    it->reader = reader;
    it->backend = reader->schema.backend;
    size_t n = reader->schema.backend == FCE_BACKEND_RADIX ? (size_t)reader->manifest.record_count : reader->index_blob.size / sizeof(SortedEntry);
    it->matches = (size_t *)fce_arena_alloc(it->arena, (n ? n : 1) * sizeof(size_t), sizeof(size_t));
    if (!it->matches) {
        fce_iterator_close(it);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    if (reader->schema.backend != FCE_BACKEND_SORTED_INDEX && reader->schema.backend != FCE_BACKEND_RADIX) {
        fce_iterator_close(it);
        return FCE_ERR_UNSUPPORTED;
    }
    if (reader->schema.backend == FCE_BACKEND_RADIX) {
        RadixHeader *h;
        RadixNode *nodes;
        RadixEdge *edges;
        RadixValue *values;
        if (!radix_parts(reader, &h, &nodes, &edges, &values)) {
            fce_iterator_close(it);
            return FCE_ERR_CORRUPT;
        }
        (void)values;
        uint32_t node = 0;
        const uint8_t *p = (const uint8_t *)prefix;
        for (size_t i = 0; i < prefix_len; i++) {
            if (!radix_find_child(nodes, edges, node, p[i], &node)) {
                *out_iterator = it;
                return FCE_OK;
            }
        }
        size_t *stack = (size_t *)fce_arena_alloc(it->arena, h->node_count * sizeof(size_t), sizeof(size_t));
        if (!stack) {
            fce_iterator_close(it);
            return FCE_ERR_OUT_OF_MEMORY;
        }
        size_t sp = 0;
        stack[sp++] = node;
        while (sp) {
            uint32_t cur = (uint32_t)stack[--sp];
            if (nodes[cur].value_index != FCE_RADIX_NO_VALUE) it->matches[it->count++] = nodes[cur].value_index;
            for (uint32_t j = 0; j < nodes[cur].edge_count; j++) {
                stack[sp++] = edges[nodes[cur].first_edge + j].child_index;
            }
        }
    } else {
        SortedEntry *e = (SortedEntry *)reader->index_blob.data;
        for (size_t i = 0; i < n; i++) {
            if (e[i].key_len >= prefix_len && range_ok(e[i].key_offset, e[i].key_len, reader->keys_blob.size) &&
                (prefix_len == 0 || memcmp(reader->keys_blob.data + e[i].key_offset, prefix, prefix_len) == 0)) {
                it->matches[it->count++] = i;
            }
        }
    }
    *out_iterator = it;
    return FCE_OK;
}

FceStatus fce_reader_range_scan(FceReader *reader, const void *start_key, size_t start_key_len, const void *end_key, size_t end_key_len, FceIterator **out_iterator) {
    if (!reader || (!start_key && start_key_len) || (!end_key && end_key_len) || !out_iterator) return FCE_ERR_INVALID_ARGUMENT;
    if (reader->schema.backend != FCE_BACKEND_SORTED_INDEX && reader->schema.backend != FCE_BACKEND_RADIX) return FCE_ERR_UNSUPPORTED;
    if (reader->schema.key_codec != FCE_CODEC_NONE && reader->schema.key_codec != FCE_CODEC_USER_BYTES) return FCE_ERR_UNSUPPORTED;
    *out_iterator = NULL;
    FceIterator *it = (FceIterator *)fce_xcalloc(1, sizeof(*it));
    if (!it) return FCE_ERR_OUT_OF_MEMORY;
    FceStatus st = fce_arena_create(&it->arena);
    if (st != FCE_OK) {
        fce_free(it);
        return st;
    }
    it->reader = reader;
    it->backend = reader->schema.backend;
    size_t n = reader->schema.backend == FCE_BACKEND_RADIX ? (size_t)reader->manifest.record_count : reader->index_blob.size / sizeof(SortedEntry);
    it->matches = (size_t *)fce_arena_alloc(it->arena, (n ? n : 1) * sizeof(size_t), sizeof(size_t));
    if (!it->matches) {
        fce_iterator_close(it);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    const uint8_t *start = (const uint8_t *)start_key;
    const uint8_t *end = (const uint8_t *)end_key;
    if (reader->schema.backend == FCE_BACKEND_RADIX) {
        RadixHeader *h;
        RadixNode *nodes;
        RadixEdge *edges;
        RadixValue *values;
        if (!radix_parts(reader, &h, &nodes, &edges, &values)) {
            fce_iterator_close(it);
            return FCE_ERR_CORRUPT;
        }
        (void)nodes;
        (void)edges;
        for (size_t i = 0; i < h->value_count; i++) {
            RadixValue *v = &values[i];
            const uint8_t *k = reader->keys_blob.data + v->key_offset;
            if (key_bytes_cmp(k, v->key_len, start, start_key_len) >= 0 &&
                key_bytes_cmp(k, v->key_len, end, end_key_len) < 0) {
                it->matches[it->count++] = i;
            }
        }
    } else {
        SortedEntry *e = (SortedEntry *)reader->index_blob.data;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *k = reader->keys_blob.data + e[i].key_offset;
            if (key_bytes_cmp(k, e[i].key_len, start, start_key_len) >= 0 &&
                key_bytes_cmp(k, e[i].key_len, end, end_key_len) < 0) {
                it->matches[it->count++] = i;
            }
        }
    }
    *out_iterator = it;
    return FCE_OK;
}

FceStatus fce_reader_scan_all(FceReader *reader, FceIterator **out_iterator) {
    if (!reader || !out_iterator) return FCE_ERR_INVALID_ARGUMENT;
    *out_iterator = NULL;
    FceIterator *it = (FceIterator *)fce_xcalloc(1, sizeof(*it));
    if (!it) return FCE_ERR_OUT_OF_MEMORY;
    FceStatus st = fce_arena_create(&it->arena);
    if (st != FCE_OK) {
        fce_free(it);
        return st;
    }
    it->reader = reader;
    it->backend = reader->schema.backend;
    size_t cap = 0;
    if (reader->schema.backend == FCE_BACKEND_SORTED_INDEX) cap = reader->index_blob.size / sizeof(SortedEntry);
    else if (reader->schema.backend == FCE_BACKEND_RADIX) cap = (size_t)reader->manifest.record_count;
    else if (reader->schema.backend == FCE_BACKEND_DIRECT_TABLE) cap = reader->index_blob.size / sizeof(DirectSlot);
    else if (reader->schema.backend == FCE_BACKEND_MPH) cap = (size_t)reader->manifest.record_count;
    else {
        fce_iterator_close(it);
        return FCE_ERR_UNSUPPORTED;
    }
    it->matches = (size_t *)fce_arena_alloc(it->arena, (cap ? cap : 1) * sizeof(size_t), sizeof(size_t));
    if (!it->matches) {
        fce_iterator_close(it);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    if (reader->schema.backend == FCE_BACKEND_SORTED_INDEX) {
        for (size_t i = 0; i < cap; i++) it->matches[it->count++] = i;
    } else if (reader->schema.backend == FCE_BACKEND_RADIX) {
        for (size_t i = 0; i < cap; i++) it->matches[it->count++] = i;
    } else if (reader->schema.backend == FCE_BACKEND_DIRECT_TABLE) {
        DirectSlot *slots = (DirectSlot *)reader->index_blob.data;
        for (size_t i = 0; i < cap; i++) if (slots[i].present) it->matches[it->count++] = i;
    } else if (reader->schema.backend == FCE_BACKEND_MPH) {
        MphHeader *h = (MphHeader *)reader->index_blob.data;
        MphSlot *slots = (MphSlot *)(reader->index_blob.data + h->slots_offset);
        for (size_t i = 0; i < h->slot_count; i++) if (slots[i].present) it->matches[it->count++] = i;
    }
    *out_iterator = it;
    return FCE_OK;
}

FceStatus fce_iterator_next(FceIterator *it, const void **out_key, size_t *out_key_len, const void **out_value, size_t *out_value_len) {
    if (!it || !out_key || !out_key_len || !out_value || !out_value_len) return FCE_ERR_INVALID_ARGUMENT;
    if (it->pos >= it->count) return FCE_ERR_NOT_FOUND;
    const void *raw_value = NULL;
    size_t raw_value_len = 0;
    if (it->backend == FCE_BACKEND_RADIX) {
        RadixHeader *h;
        RadixNode *nodes;
        RadixEdge *edges;
        RadixValue *values;
        if (!radix_parts(it->reader, &h, &nodes, &edges, &values)) return FCE_ERR_CORRUPT;
        (void)h;
        (void)nodes;
        (void)edges;
        RadixValue *cur = &values[it->matches[it->pos++]];
        if (!range_ok(cur->key_offset, cur->key_len, it->reader->keys_blob.size) ||
            !range_ok(cur->value_offset, cur->value_len, it->reader->values_blob.size)) return FCE_ERR_CORRUPT;
        *out_key = it->reader->keys_blob.data + cur->key_offset;
        *out_key_len = cur->key_len;
        raw_value = it->reader->values_blob.data + cur->value_offset;
        raw_value_len = cur->value_len;
    } else if (it->backend == FCE_BACKEND_SORTED_INDEX) {
        SortedEntry *e = (SortedEntry *)it->reader->index_blob.data;
        SortedEntry *cur = &e[it->matches[it->pos++]];
        if (!range_ok(cur->key_offset, cur->key_len, it->reader->keys_blob.size) ||
            !range_ok(cur->value_offset, cur->value_len, it->reader->values_blob.size)) return FCE_ERR_CORRUPT;
        *out_key = it->reader->keys_blob.data + cur->key_offset;
        *out_key_len = cur->key_len;
        raw_value = it->reader->values_blob.data + cur->value_offset;
        raw_value_len = cur->value_len;
    } else if (it->backend == FCE_BACKEND_DIRECT_TABLE) {
        DirectSlot *slots = (DirectSlot *)it->reader->index_blob.data;
        DirectSlot *cur = &slots[it->matches[it->pos++]];
        if (!cur->present || !range_ok(cur->key_offset, cur->key_len, it->reader->keys_blob.size) ||
            !range_ok(cur->value_offset, cur->value_len, it->reader->values_blob.size)) return FCE_ERR_CORRUPT;
        *out_key = it->reader->keys_blob.data + cur->key_offset;
        *out_key_len = cur->key_len;
        raw_value = it->reader->values_blob.data + cur->value_offset;
        raw_value_len = cur->value_len;
    } else if (it->backend == FCE_BACKEND_MPH) {
        MphHeader *h = (MphHeader *)it->reader->index_blob.data;
        MphSlot *slots = (MphSlot *)(it->reader->index_blob.data + h->slots_offset);
        MphSlot *cur = &slots[it->matches[it->pos++]];
        if (!cur->present || !range_ok(cur->key_offset, cur->key_len, it->reader->keys_blob.size) ||
            !range_ok(cur->value_offset, cur->value_len, it->reader->values_blob.size)) return FCE_ERR_CORRUPT;
        *out_key = it->reader->keys_blob.data + cur->key_offset;
        *out_key_len = cur->key_len;
        raw_value = it->reader->values_blob.data + cur->value_offset;
        raw_value_len = cur->value_len;
    } else {
        return FCE_ERR_UNSUPPORTED;
    }
    if (it->reader->schema.value_codec == FCE_CODEC_NONE || it->reader->schema.value_codec == FCE_CODEC_USER_BYTES) {
        *out_value = raw_value;
        *out_value_len = raw_value_len;
        return FCE_OK;
    }
    void *decoded = NULL;
    size_t decoded_len = 0;
    FceStatus st = fce_codec_decode(it->reader->schema.value_codec, raw_value, raw_value_len, &decoded, &decoded_len);
    if (st != FCE_OK) return st;
    void *owned = fce_arena_memdup(it->arena, decoded, decoded_len);
    fce_free(decoded);
    if (!owned) return FCE_ERR_OUT_OF_MEMORY;
    *out_value = owned;
    *out_value_len = decoded_len;
    return FCE_OK;
}

void fce_iterator_close(FceIterator *it) {
    if (!it) return;
    fce_arena_destroy(it->arena);
    fce_free(it);
}

void fce_reader_close(FceReader *reader) {
    if (!reader) return;
    fce_arena_destroy(reader->arena);
    if (reader->has_cache_lock) {
        fce_cache_lock_release(&reader->cache_lock);
        reader->has_cache_lock = 0;
    }
    fce_free(reader);
}

FceStatus fce_inspect(const char *cache_dir, FceManifestInfo *out_info) {
    if (!cache_dir || !out_info) return FCE_ERR_INVALID_ARGUMENT;
    FceFileLock lock;
    FceStatus st = fce_cache_lock_acquire_shared(cache_dir, &lock);
    if (st != FCE_OK) return st;
    st = read_manifest(cache_dir, out_info);
    fce_cache_lock_release(&lock);
    return st;
}

FceStatus fce_validate(const char *cache_dir, const FceSchema *expected_schema) {
    FceReader *r = NULL;
    FceStatus st = fce_reader_open_expected(cache_dir, expected_schema, &r);
    fce_reader_close(r);
    return st;
}

FceStatus fce_compact(const char *source_cache_dir, FceBackendKind backend, const char *output_cache_dir) {
    if (!source_cache_dir || !output_cache_dir) return FCE_ERR_INVALID_ARGUMENT;
    FceReader *r = NULL;
    FceStatus st = fce_reader_open(source_cache_dir, &r);
    if (st != FCE_OK) return st;
    FceSchema s = r->schema;
    s.backend = backend;
    FceBuilder *b = NULL;
    st = fce_builder_open(output_cache_dir, &s, &b);
    if (st == FCE_OK && r->schema.backend == FCE_BACKEND_LOG) {
        size_t pos = 0;
        while (pos + 32 <= r->log_blob.size && st == FCE_OK) {
            uint64_t kl = rd64(r->log_blob.data + pos + 8);
            uint64_t vl = rd64(r->log_blob.data + pos + 16);
            pos += 24;
            uint8_t *kp = r->log_blob.data + pos;
            pos += (size_t)kl;
            uint8_t *vp = r->log_blob.data + pos;
            pos += (size_t)vl + 8;
            void *decoded_key = NULL;
            void *decoded_value = NULL;
            size_t decoded_key_len = 0;
            size_t decoded_value_len = 0;
            st = fce_codec_decode(r->schema.key_codec, kp, (size_t)kl, &decoded_key, &decoded_key_len);
            if (st == FCE_OK) st = fce_codec_decode(r->schema.value_codec, vp, (size_t)vl, &decoded_value, &decoded_value_len);
            if (st == FCE_OK) st = fce_builder_put(b, decoded_key, decoded_key_len, decoded_value, decoded_value_len);
            fce_free(decoded_key);
            fce_free(decoded_value);
        }
    } else if (st == FCE_OK) {
        FceIterator *it = NULL;
        st = fce_reader_scan_all(r, &it);
        while (st == FCE_OK) {
            const void *k, *v;
            size_t kl, vl;
            FceStatus nx = fce_iterator_next(it, &k, &kl, &v, &vl);
            if (nx == FCE_ERR_NOT_FOUND) break;
            if (nx != FCE_OK) {
                st = nx;
                break;
            }
            st = fce_builder_put(b, k, kl, v, vl);
        }
        fce_iterator_close(it);
    }
    fce_reader_close(r);
    r = NULL;
    if (st == FCE_OK) st = fce_builder_freeze(b);
    fce_builder_close(b);
    return st;
}
