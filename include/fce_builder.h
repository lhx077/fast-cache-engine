#ifndef FCE_BUILDER_H
#define FCE_BUILDER_H

#include "fce_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

FCE_API FceStatus fce_builder_open(
    const char *cache_dir,
    const FceSchema *schema,
    FceBuilder **out_builder);
FCE_API FceStatus fce_builder_put(
    FceBuilder *builder,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
FCE_API FceStatus fce_builder_put_u64(
    FceBuilder *builder,
    uint64_t key,
    const void *value,
    size_t value_len);
FCE_API FceStatus fce_builder_put_u128(
    FceBuilder *builder,
    uint64_t key_lo,
    uint64_t key_hi,
    const void *value,
    size_t value_len);
FCE_API FceStatus fce_builder_bulk_put(
    FceBuilder *builder,
    const void *const *keys,
    const size_t *key_lens,
    const void *const *values,
    const size_t *value_lens,
    size_t count);
FCE_API FceStatus fce_builder_freeze(FceBuilder *builder);
FCE_API FceStatus fce_builder_close(FceBuilder *builder);

FCE_API FceStatus fce_log_append(
    const char *cache_dir,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
FCE_API FceStatus fce_compact(
    const char *source_cache_dir,
    FceBackendKind backend,
    const char *output_cache_dir);

#ifdef __cplusplus
}
#endif

#endif
