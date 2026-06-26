#ifndef FCE_CACHE_H
#define FCE_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(FCE_BUILD_SHARED)
#  if defined(FCE_BUILDING_LIBRARY)
#    define FCE_API __declspec(dllexport)
#  else
#    define FCE_API __declspec(dllimport)
#  endif
#else
#  define FCE_API
#endif

#if defined(__clang__)
#  define FCE_OWNERSHIP_RETURNS(kind) __attribute__((ownership_returns(kind)))
#  define FCE_OWNERSHIP_TAKES(kind, index) __attribute__((ownership_takes(kind, index)))
#else
#  define FCE_OWNERSHIP_RETURNS(kind)
#  define FCE_OWNERSHIP_TAKES(kind, index)
#endif

#define FCE_ABI_VERSION_MAJOR 1u
#define FCE_ABI_VERSION_MINOR 0u
#define FCE_ABI_VERSION_PATCH 0u
#define FCE_ABI_VERSION ((FCE_ABI_VERSION_MAJOR << 16) | (FCE_ABI_VERSION_MINOR << 8) | FCE_ABI_VERSION_PATCH)

typedef struct FceBuilder FceBuilder;
typedef struct FceReader FceReader;
typedef struct FceIterator FceIterator;

typedef enum {
    FCE_OK = 0,
    FCE_ERR_INVALID_ARGUMENT = 1,
    FCE_ERR_IO = 2,
    FCE_ERR_CORRUPT = 3,
    FCE_ERR_NOT_FOUND = 4,
    FCE_ERR_UNSUPPORTED = 5,
    FCE_ERR_SCHEMA_MISMATCH = 6,
    FCE_ERR_OUT_OF_MEMORY = 7
} FceStatus;

typedef enum {
    FCE_BACKEND_SORTED_INDEX = 1,
    FCE_BACKEND_MPH = 2,
    FCE_BACKEND_DIRECT_TABLE = 3,
    FCE_BACKEND_RADIX = 4,
    FCE_BACKEND_LOG = 5
} FceBackendKind;

typedef enum {
    FCE_LOOKUP_EXACT = 1,
    FCE_LOOKUP_PREFIX = 2,
    FCE_LOOKUP_RANGE = 3
} FceLookupKind;

typedef enum {
    FCE_KEY_BYTES = 1,
    FCE_KEY_U64 = 2,
    FCE_KEY_U128 = 3,
    FCE_KEY_INT_TUPLE = 4,
    FCE_KEY_SEQUENCE = 5
} FceKeyKind;

typedef enum {
    FCE_VALUE_BYTES = 1,
    FCE_VALUE_FIXED_RECORD = 2,
    FCE_VALUE_VAR_RECORD = 3
} FceValueKind;

typedef enum {
    FCE_CODEC_NONE = 0,
    FCE_CODEC_DELTA_I32 = 1,
    FCE_CODEC_RLE = 2,
    FCE_CODEC_BITPACK = 3,
    FCE_CODEC_USER_BYTES = 4,
    FCE_CODEC_ZSTD = 5
} FceCodecKind;

enum {
    FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE = 1u << 0,
    FCE_FLAG_FORCE_HASH_COLLISION = 1u << 1
};

typedef struct {
    uint32_t schema_version;
    uint32_t algorithm_version;
    FceBackendKind backend;
    FceLookupKind lookup;
    FceKeyKind key_kind;
    FceValueKind value_kind;
    FceCodecKind key_codec;
    FceCodecKind value_codec;
    uint32_t fixed_key_size;
    uint32_t fixed_value_size;
    uint32_t direct_table_key_bits;
    uint32_t user_flags;
    uint8_t exact_key_check;
    uint8_t mmap_read;
    uint8_t read_only_after_freeze;
    uint8_t allow_duplicate_put;
} FceSchema;

typedef struct {
    char magic[8];
    uint32_t manifest_version;
    uint32_t schema_version;
    uint32_t algorithm_version;
    uint32_t backend_kind;
    uint32_t lookup_kind;
    uint32_t key_kind;
    uint32_t value_kind;
    uint32_t key_codec;
    uint32_t value_codec;
    uint64_t record_count;
    uint64_t index_size;
    uint64_t keys_size;
    uint64_t values_size;
    uint64_t log_size;
    uint64_t build_unix_time;
    uint64_t schema_hash;
    uint64_t index_crc64;
    uint64_t keys_crc64;
    uint64_t values_crc64;
    uint64_t log_crc64;
    uint64_t endian_tag;
    uint64_t backend_meta0;
    uint64_t backend_meta1;
    uint32_t fixed_key_size;
    uint32_t fixed_value_size;
    uint32_t direct_table_key_bits;
    uint32_t reserved0;
} FceManifestInfo;

typedef struct {
    uint64_t active_allocations;
    uint64_t active_bytes;
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t peak_bytes;
} FceMemoryStats;

typedef struct {
    uint64_t estimated_record_count;
    uint64_t dense_u64_max_key;
    uint32_t average_key_size;
    uint32_t average_value_size;
    FceLookupKind lookup;
    FceKeyKind key_kind;
    uint8_t needs_online_append;
    uint8_t prefers_prefix_scan;
    uint8_t read_mostly;
    uint8_t allow_mph;
} FcePlannerInput;

FCE_API const char *fce_status_string(FceStatus status);
FCE_API uint32_t fce_abi_version(void);
FCE_API const char *fce_version_string(void);
FCE_API FceSchema fce_schema_default(void);
FCE_API FceStatus fce_schema_plan(const FcePlannerInput *input, FceSchema *out_schema);
FCE_API uint64_t fce_schema_hash(const FceSchema *schema);
FCE_API FceStatus fce_memory_stats(FceMemoryStats *out_stats);

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

FCE_API FceStatus fce_codec_encode(
    FceCodecKind codec,
    const void *input,
    size_t input_len,
    void **out_data,
    size_t *out_len);
FCE_API FceStatus fce_codec_decode(
    FceCodecKind codec,
    const void *input,
    size_t input_len,
    void **out_data,
    size_t *out_len);
FCE_API int fce_codec_available(FceCodecKind codec);
FCE_API void fce_free(void *ptr) FCE_OWNERSHIP_TAKES(malloc, 1);

#ifdef __cplusplus
}
#endif

#endif
