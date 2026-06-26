#include "../internal/fce_internal.h"

static int cmp_mph_bucket_desc(const void *a, const void *b) {
    const MphBuildBucket *x = (const MphBuildBucket *)a;
    const MphBuildBucket *y = (const MphBuildBucket *)b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return x->index < y->index ? -1 : (x->index > y->index);
}

FceStatus freeze_mph(FceBuilder *b) {
    BuildRecord *records = NULL;
    size_t count = 0;
    FceStatus st = dedupe_records(b, &records, &count);
    if (st != FCE_OK) return st;
    size_t bucket_count = count;
    MphBucket *buckets = (MphBucket *)fce_xcalloc(bucket_count ? bucket_count : 1, sizeof(*buckets));
    MphSlot *slots = (MphSlot *)fce_xcalloc(count ? count : 1, sizeof(*slots));
    SortedEntry *entries = (SortedEntry *)fce_xcalloc(count ? count : 1, sizeof(*entries));
    if (!buckets || !slots || !entries) {
        fce_free(buckets);
        fce_free(slots);
        fce_free(entries);
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    uint64_t keys_size = 0, values_size = 0;
    st = write_keys_values(b, records, count, entries, &keys_size, &values_size);
    if (st == FCE_OK) {
        uint32_t *bucket_sizes = (uint32_t *)fce_xcalloc(bucket_count ? bucket_count : 1, sizeof(*bucket_sizes));
        uint32_t *bucket_offsets = (uint32_t *)fce_xcalloc(bucket_count ? bucket_count : 1, sizeof(*bucket_offsets));
        uint32_t *bucket_pos = (uint32_t *)fce_xcalloc(bucket_count ? bucket_count : 1, sizeof(*bucket_pos));
        uint32_t *bucket_items = (uint32_t *)fce_xcalloc(count ? count : 1, sizeof(*bucket_items));
        MphBuildBucket *order = (MphBuildBucket *)fce_xcalloc(bucket_count ? bucket_count : 1, sizeof(*order));
        uint8_t *used = (uint8_t *)fce_xcalloc(count ? count : 1, 1);
        uint32_t *trial_slots = (uint32_t *)fce_xcalloc(count ? count : 1, sizeof(*trial_slots));
        if (!bucket_sizes || !bucket_offsets || !bucket_pos || !bucket_items || !order || !used || !trial_slots) st = FCE_ERR_OUT_OF_MEMORY;
        if (st == FCE_OK) {
            for (size_t i = 0; i < count; i++) {
                Hash128 h = hash_key(&b->schema, records[i].key, records[i].key_len);
                bucket_sizes[(size_t)(h.hi % bucket_count)]++;
            }
            uint32_t off = 0;
            for (size_t i = 0; i < bucket_count; i++) {
                bucket_offsets[i] = off;
                bucket_pos[i] = off;
                order[i].index = (uint32_t)i;
                order[i].count = bucket_sizes[i];
                off += bucket_sizes[i];
            }
            for (size_t i = 0; i < count; i++) {
                Hash128 h = hash_key(&b->schema, records[i].key, records[i].key_len);
                uint32_t bucket = (uint32_t)(h.hi % bucket_count);
                bucket_items[bucket_pos[bucket]++] = (uint32_t)i;
            }
            qsort(order, bucket_count, sizeof(*order), cmp_mph_bucket_desc);
            for (size_t oi = 0; oi < bucket_count && st == FCE_OK; oi++) {
                uint32_t bucket = order[oi].index;
                uint32_t bcount = order[oi].count;
                if (!bcount) {
                    buckets[bucket].seed = 0;
                    continue;
                }
                int placed = 0;
                for (uint64_t seed = 1; seed < 1000000ULL && !placed; seed++) {
                    placed = 1;
                    for (uint32_t j = 0; j < bcount; j++) {
                        uint32_t rec_index = bucket_items[bucket_offsets[bucket] + j];
                        uint32_t slot = (uint32_t)(fnv1a64(records[rec_index].key, records[rec_index].key_len, seed) % count);
                        trial_slots[j] = slot;
                        if (used[slot]) {
                            placed = 0;
                            break;
                        }
                        for (uint32_t k = 0; k < j; k++) {
                            if (trial_slots[k] == slot) {
                                placed = 0;
                                break;
                            }
                        }
                        if (!placed) break;
                    }
                    if (placed) {
                        buckets[bucket].seed = seed;
                        for (uint32_t j = 0; j < bcount; j++) {
                            uint32_t rec_index = bucket_items[bucket_offsets[bucket] + j];
                            uint32_t slot = trial_slots[j];
                            Hash128 h = hash_key(&b->schema, records[rec_index].key, records[rec_index].key_len);
                            used[slot] = 1;
                            slots[slot].present = 1;
                            slots[slot].hash_lo = h.lo;
                            slots[slot].hash_hi = h.hi;
                            slots[slot].key_offset = entries[rec_index].key_offset;
                            slots[slot].key_len = entries[rec_index].key_len;
                            slots[slot].value_offset = entries[rec_index].value_offset;
                            slots[slot].value_len = entries[rec_index].value_len;
                        }
                    }
                }
                if (!placed) st = FCE_ERR_UNSUPPORTED;
            }
        }
        fce_free(bucket_sizes);
        fce_free(bucket_offsets);
        fce_free(bucket_pos);
        fce_free(bucket_items);
        fce_free(order);
        fce_free(used);
        fce_free(trial_slots);
    }
    if (st == FCE_OK) {
        size_t total = sizeof(MphHeader) + bucket_count * sizeof(MphBucket) + count * sizeof(MphSlot);
        uint8_t *buf = (uint8_t *)fce_xcalloc(1, total ? total : 1);
        if (!buf) st = FCE_ERR_OUT_OF_MEMORY;
        else {
            MphHeader *h = (MphHeader *)buf;
            h->magic = FCE_MPH_MAGIC;
            h->bucket_count = bucket_count;
            h->slot_count = count;
            h->buckets_offset = sizeof(MphHeader);
            h->slots_offset = sizeof(MphHeader) + bucket_count * sizeof(MphBucket);
            memcpy(buf + h->buckets_offset, buckets, bucket_count * sizeof(MphBucket));
            memcpy(buf + h->slots_offset, slots, count * sizeof(MphSlot));
            char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
            st = ip ? write_file(ip, buf, total) : FCE_ERR_OUT_OF_MEMORY;
            fce_free(buf);
        }
    }
    if (st == FCE_OK) {
        char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
        FceManifestInfo m = make_manifest(&b->schema, (uint64_t)count);
        m.index_size = sizeof(MphHeader) + bucket_count * sizeof(MphBucket) + count * sizeof(MphSlot);
        m.keys_size = keys_size;
        m.values_size = values_size;
        m.backend_meta0 = bucket_count;
        char *kp = join_path_arena(b->arena, b->cache_dir, FCE_KEYS_FILE);
        char *vp = join_path_arena(b->arena, b->cache_dir, FCE_VALUES_FILE);
        m.index_crc64 = crc64_file(ip);
        m.keys_crc64 = crc64_file(kp);
        m.values_crc64 = crc64_file(vp);
        st = write_manifest(b->cache_dir, &m);
    }
    fce_free(buckets);
    fce_free(slots);
    fce_free(entries);
    fce_free(records);
    return st;
}

FceStatus get_mph(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    if (r->index_blob.size < sizeof(MphHeader)) return FCE_ERR_CORRUPT;
    MphHeader *mh = (MphHeader *)r->index_blob.data;
    if (mh->magic != FCE_MPH_MAGIC) return FCE_ERR_CORRUPT;
    if (!mh->slot_count || !mh->bucket_count) return FCE_ERR_NOT_FOUND;
    MphBucket *buckets = (MphBucket *)(r->index_blob.data + mh->buckets_offset);
    MphSlot *s = (MphSlot *)(r->index_blob.data + mh->slots_offset);
    Hash128 h = hash_key(&r->schema, key, key_len);
    uint64_t bucket = h.hi % mh->bucket_count;
    uint64_t seed = buckets[bucket].seed;
    if (!seed) return FCE_ERR_NOT_FOUND;
    uint64_t slot = fnv1a64(key, key_len, seed) % mh->slot_count;
    MphSlot *m = &s[slot];
    if (m->present && m->hash_lo == h.lo && m->hash_hi == h.hi &&
        key_equal(r->keys_blob.data, r->keys_blob.size, m->key_offset, m->key_len, key, key_len)) {
        if (!range_ok(m->value_offset, m->value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
        *out_value = r->values_blob.data + m->value_offset;
        *out_value_len = m->value_len;
        return FCE_OK;
    }
    return FCE_ERR_NOT_FOUND;
}

