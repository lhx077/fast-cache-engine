#ifndef FCE_READER_H
#define FCE_READER_H

#include "fce_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

FCE_API FceStatus fce_reader_open(
    const char *cache_dir,
    FceReader **out_reader);
FCE_API FceStatus fce_reader_open_expected(
    const char *cache_dir,
    const FceSchema *expected_schema,
    FceReader **out_reader);
FCE_API FceStatus fce_reader_get(
    FceReader *reader,
    const void *key,
    size_t key_len,
    const void **out_value,
    size_t *out_value_len);
FCE_API FceStatus fce_reader_get_u64(
    FceReader *reader,
    uint64_t key,
    const void **out_value,
    size_t *out_value_len);
FCE_API FceStatus fce_reader_get_u128(
    FceReader *reader,
    uint64_t key_lo,
    uint64_t key_hi,
    const void **out_value,
    size_t *out_value_len);
FCE_API FceStatus fce_reader_get_copy(
    FceReader *reader,
    const void *key,
    size_t key_len,
    void *out_value,
    size_t *inout_value_len);
FCE_API FceStatus fce_reader_get_batch(
    FceReader *reader,
    const void *const *keys,
    const size_t *key_lens,
    const void **out_values,
    size_t *out_value_lens,
    FceStatus *out_statuses,
    size_t count);
FCE_API FceStatus fce_reader_prefix_scan(
    FceReader *reader,
    const void *prefix,
    size_t prefix_len,
    FceIterator **out_iterator);
FCE_API FceStatus fce_reader_range_scan(
    FceReader *reader,
    const void *start_key,
    size_t start_key_len,
    const void *end_key,
    size_t end_key_len,
    FceIterator **out_iterator);
FCE_API FceStatus fce_reader_scan_all(
    FceReader *reader,
    FceIterator **out_iterator);
FCE_API FceStatus fce_iterator_next(
    FceIterator *it,
    const void **out_key,
    size_t *out_key_len,
    const void **out_value,
    size_t *out_value_len);
FCE_API void fce_iterator_close(FceIterator *it);
FCE_API void fce_reader_close(FceReader *reader);

FCE_API FceStatus fce_inspect(const char *cache_dir, FceManifestInfo *out_info);
FCE_API FceStatus fce_validate(const char *cache_dir, const FceSchema *expected_schema);

#ifdef __cplusplus
}
#endif

#endif
