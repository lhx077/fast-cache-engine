#include "../internal/fce_internal.h"

FceManifestInfo make_manifest(const FceSchema *schema, uint64_t records) {
    FceManifestInfo m;
    memset(&m, 0, sizeof(m));
    memcpy(m.magic, FCE_MAGIC, 8);
    m.manifest_version = 1;
    m.schema_version = schema->schema_version;
    m.algorithm_version = schema->algorithm_version;
    m.backend_kind = (uint32_t)schema->backend;
    m.lookup_kind = (uint32_t)schema->lookup;
    m.key_kind = (uint32_t)schema->key_kind;
    m.value_kind = (uint32_t)schema->value_kind;
    m.key_codec = (uint32_t)schema->key_codec;
    m.value_codec = (uint32_t)schema->value_codec;
    m.record_count = records;
    m.build_unix_time = (uint64_t)time(NULL);
    m.schema_hash = fce_schema_hash(schema);
    m.endian_tag = FCE_ENDIAN_TAG;
    m.fixed_key_size = schema->fixed_key_size;
    m.fixed_value_size = schema->fixed_value_size;
    m.direct_table_key_bits = schema->direct_table_key_bits;
    m.backend_meta1 = schema->user_flags;
    if (schema->exact_key_check) m.backend_meta1 |= FCE_META_FLAG_EXACT_KEY_CHECK;
    if (schema->mmap_read) m.backend_meta1 |= FCE_META_FLAG_MMAP_READ;
    if (schema->read_only_after_freeze) m.backend_meta1 |= FCE_META_FLAG_READ_ONLY_AFTER_FREEZE;
    if (schema->allow_duplicate_put) m.backend_meta1 |= FCE_META_FLAG_ALLOW_DUPLICATE_PUT;
    return m;
}

FceSchema schema_from_manifest(const FceManifestInfo *m) {
    FceSchema s = fce_schema_default();
    s.schema_version = m->schema_version;
    s.algorithm_version = m->algorithm_version;
    s.backend = (FceBackendKind)m->backend_kind;
    s.lookup = (FceLookupKind)m->lookup_kind;
    s.key_kind = (FceKeyKind)m->key_kind;
    s.value_kind = (FceValueKind)m->value_kind;
    s.key_codec = (FceCodecKind)m->key_codec;
    s.value_codec = (FceCodecKind)m->value_codec;
    s.fixed_key_size = m->fixed_key_size;
    s.fixed_value_size = m->fixed_value_size;
    s.direct_table_key_bits = m->direct_table_key_bits;
    s.user_flags = (uint32_t)m->backend_meta1;
    s.exact_key_check = (m->backend_meta1 & FCE_META_FLAG_EXACT_KEY_CHECK) ? 1 : 0;
    s.mmap_read = (m->backend_meta1 & FCE_META_FLAG_MMAP_READ) ? 1 : 0;
    s.read_only_after_freeze = (m->backend_meta1 & FCE_META_FLAG_READ_ONLY_AFTER_FREEZE) ? 1 : 0;
    s.allow_duplicate_put = (m->backend_meta1 & FCE_META_FLAG_ALLOW_DUPLICATE_PUT) ? 1 : 0;
    return s;
}

FceStatus read_manifest(const char *cache_dir, FceManifestInfo *out) {
    if (!cache_dir || !out) return FCE_ERR_INVALID_ARGUMENT;
    char *path = join_path_heap(cache_dir, FCE_MANIFEST_FILE);
    if (!path) return FCE_ERR_OUT_OF_MEMORY;
    FILE *f = fopen(path, "rb");
    fce_free(path);
    if (!f) return FCE_ERR_IO;
    FceManifestInfo m;
    if (fread(&m, 1, sizeof(m), f) != sizeof(m)) {
        fclose(f);
        return FCE_ERR_CORRUPT;
    }
    fclose(f);
    if (memcmp(m.magic, FCE_MAGIC, 8) != 0 || m.endian_tag != FCE_ENDIAN_TAG) return FCE_ERR_CORRUPT;
    if (m.manifest_version != 1 || m.schema_version != 1 || m.algorithm_version != 1) return FCE_ERR_SCHEMA_MISMATCH;
    FceSchema s = schema_from_manifest(&m);
    FceStatus st = validate_schema(&s);
    if (st != FCE_OK) return st;
    *out = m;
    return FCE_OK;
}

FceStatus write_manifest(const char *cache_dir, FceManifestInfo *m) {
    char *path = join_path_heap(cache_dir, FCE_MANIFEST_FILE);
    if (!path) return FCE_ERR_OUT_OF_MEMORY;
    FceStatus st = write_file(path, m, sizeof(*m));
    fce_free(path);
    return st;
}
