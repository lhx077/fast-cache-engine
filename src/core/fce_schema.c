#include "../internal/fce_internal.h"

const char *fce_status_string(FceStatus status) {
    switch (status) {
        case FCE_OK: return "ok";
        case FCE_ERR_INVALID_ARGUMENT: return "invalid argument";
        case FCE_ERR_IO: return "io error";
        case FCE_ERR_CORRUPT: return "corrupt cache";
        case FCE_ERR_NOT_FOUND: return "not found";
        case FCE_ERR_UNSUPPORTED: return "unsupported";
        case FCE_ERR_SCHEMA_MISMATCH: return "schema mismatch";
        case FCE_ERR_OUT_OF_MEMORY: return "out of memory";
        default: return "unknown status";
    }
}

FceSchema fce_schema_default(void) {
    FceSchema s;
    memset(&s, 0, sizeof(s));
    s.schema_version = 1;
    s.algorithm_version = 1;
    s.backend = FCE_BACKEND_SORTED_INDEX;
    s.lookup = FCE_LOOKUP_EXACT;
    s.key_kind = FCE_KEY_BYTES;
    s.value_kind = FCE_VALUE_VAR_RECORD;
    s.key_codec = FCE_CODEC_NONE;
    s.value_codec = FCE_CODEC_NONE;
    s.exact_key_check = 1;
    s.mmap_read = 1;
    s.read_only_after_freeze = 1;
    return s;
}

uint64_t fce_schema_hash(const FceSchema *schema) {
    if (!schema) return 0;
    return fnv1a64(schema, sizeof(*schema), 0xfeedfacecafebeefULL);
}

FceStatus validate_schema(const FceSchema *s) {
    if (!s) return FCE_ERR_INVALID_ARGUMENT;
    if (s->backend < FCE_BACKEND_SORTED_INDEX || s->backend > FCE_BACKEND_LOG) return FCE_ERR_UNSUPPORTED;
    if (s->lookup < FCE_LOOKUP_EXACT || s->lookup > FCE_LOOKUP_RANGE) return FCE_ERR_UNSUPPORTED;
    if (s->key_kind < FCE_KEY_BYTES || s->key_kind > FCE_KEY_SEQUENCE) return FCE_ERR_UNSUPPORTED;
    if (s->value_kind < FCE_VALUE_BYTES || s->value_kind > FCE_VALUE_VAR_RECORD) return FCE_ERR_UNSUPPORTED;
    if (s->key_codec > FCE_CODEC_ZSTD || s->value_codec > FCE_CODEC_ZSTD) return FCE_ERR_UNSUPPORTED;
    if (s->value_kind == FCE_VALUE_FIXED_RECORD && s->fixed_value_size == 0) return FCE_ERR_INVALID_ARGUMENT;
    if (s->fixed_key_size && s->key_kind == FCE_KEY_U64 && s->fixed_key_size != 8) return FCE_ERR_INVALID_ARGUMENT;
    if (s->fixed_key_size && s->key_kind == FCE_KEY_U128 && s->fixed_key_size != 16) return FCE_ERR_INVALID_ARGUMENT;
    if (s->backend == FCE_BACKEND_DIRECT_TABLE && s->key_kind != FCE_KEY_U64) return FCE_ERR_INVALID_ARGUMENT;
    if (s->backend == FCE_BACKEND_DIRECT_TABLE && s->key_codec != FCE_CODEC_NONE && s->key_codec != FCE_CODEC_USER_BYTES) return FCE_ERR_INVALID_ARGUMENT;
    if ((s->lookup == FCE_LOOKUP_PREFIX || s->backend == FCE_BACKEND_RADIX) &&
        s->key_codec != FCE_CODEC_NONE && s->key_codec != FCE_CODEC_USER_BYTES) return FCE_ERR_INVALID_ARGUMENT;
    if (s->lookup == FCE_LOOKUP_RANGE &&
        s->key_codec != FCE_CODEC_NONE && s->key_codec != FCE_CODEC_USER_BYTES) return FCE_ERR_INVALID_ARGUMENT;
    return FCE_OK;
}
