# Fast Cache Engine API Reference

This document describes the C ABI, Python binding, CLI surface, ownership rules, error behavior, and schema semantics for Fast Cache Engine.

All public declarations live under `include/`. The primary header is `fce_cache.h`; smaller wrapper headers (`fce_builder.h`, `fce_reader.h`, `fce_schema.h`, `fce_error.h`) expose narrower include surfaces. The arena API is internal and is not installed as a public header.

## Ownership Model

Fast Cache Engine has three memory ownership categories:

- Reader-owned views: values returned by `fce_reader_get`, iterator keys, and iterator values usually point into mmap or reader/iterator arenas. They remain valid until the owning reader or iterator is closed.
- Caller-owned buffers: buffers returned by `fce_codec_encode` and `fce_codec_decode` must be released with `fce_free`.
- Opaque handles: `FceBuilder`, `FceReader`, and `FceIterator` must be closed with their matching close function.

Do not call `free` on memory returned by Fast Cache Engine. Use `fce_free` only for caller-owned buffers explicitly documented as such.

## Thread And Process Safety

Operations that touch a cache directory are serialized with an internal `cache.lock` file. Writers such as `fce_builder_freeze` and `fce_log_append` take an exclusive lock. Readers opened by `fce_reader_open` or `fce_reader_open_expected` hold a shared lock until `fce_reader_close`; `fce_validate` and `fce_inspect` also use shared locking. This prevents readers from observing half-written snapshots and prevents writers from replacing files that an mmap-backed reader is still using.

An opened reader is a snapshot view. It does not observe later appends or freezes; reopen the reader to see new records. Writers targeting the same cache directory wait for open readers to close. The internal allocator, memory statistics, and arena bookkeeping are protected for concurrent API calls. Do not close a builder, reader, or iterator while another thread is using the same handle. Mutable operations on the same builder or iterator should still be serialized by the caller.

## Status Codes

```c
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
```

- `FCE_OK`: operation succeeded.
- `FCE_ERR_INVALID_ARGUMENT`: null pointer, invalid length, unsupported key shape for the selected backend, duplicate key when duplicates are disallowed, or too-small copy buffer.
- `FCE_ERR_IO`: filesystem read/write/open/close failure.
- `FCE_ERR_CORRUPT`: persisted files failed magic, size, checksum, offset, or record validation.
- `FCE_ERR_NOT_FOUND`: lookup miss or iterator exhausted.
- `FCE_ERR_UNSUPPORTED`: selected feature combination is not supported.
- `FCE_ERR_SCHEMA_MISMATCH`: caller supplied an expected schema whose hash does not match the cache manifest.
- `FCE_ERR_OUT_OF_MEMORY`: allocation failed or a requested structure would overflow addressable size.

```c
const char *fce_status_string(FceStatus status);
uint32_t fce_abi_version(void);
const char *fce_version_string(void);
```

`fce_status_string` returns a static English string for diagnostics. The returned pointer must not be freed.

`fce_abi_version` returns the packed `FCE_ABI_VERSION` value from the public header. `fce_version_string` returns the library version string. Consumers that dynamically load the shared library should check these before binding optional symbols.

## Schema

```c
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
```

### Backend

```c
FCE_BACKEND_SORTED_INDEX
FCE_BACKEND_MPH
FCE_BACKEND_DIRECT_TABLE
FCE_BACKEND_RADIX
FCE_BACKEND_LOG
```

- `sorted_index`: default exact lookup backend. Stores sorted hash entries plus exact key bytes.
- `mph`: read-only high-frequency exact lookup using bucket seeds and fixed slots.
- `direct_table`: dense `u64` addressing. Requires `key_kind = FCE_KEY_U64`.
- `radix`: prefix/range-oriented byte-key trie stored as mmap-safe arrays.
- `log`: appendable online backend. Reader builds a transient in-memory index at open.

### Lookup Kind

```c
FCE_LOOKUP_EXACT
FCE_LOOKUP_PREFIX
FCE_LOOKUP_RANGE
```

The lookup kind documents intended use and validates some combinations. Exact lookup is available on all backends. Prefix scan is supported on sorted and radix backends. Range scan is supported on sorted and radix backends using canonical byte ordering.

### Key Kind

```c
FCE_KEY_BYTES
FCE_KEY_U64
FCE_KEY_U128
FCE_KEY_INT_TUPLE
FCE_KEY_SEQUENCE
```

`FCE_KEY_BYTES` is the general-purpose option. `FCE_KEY_U64` enables `fce_builder_put_u64`, `fce_reader_get_u64`, and direct-table lookup. `FCE_KEY_U128` has helper APIs for two little-endian `uint64_t` words. Tuple and sequence kinds are schema-level documentation for application encoders; the library still stores canonical bytes.

### Value Kind

```c
FCE_VALUE_BYTES
FCE_VALUE_FIXED_RECORD
FCE_VALUE_VAR_RECORD
```

The library stores value bytes. Fixed and variable record kinds allow applications to document expected layout. Fixed-size values can be read as direct views when no decoding is required.

When `fixed_key_size` is nonzero, `fce_builder_put` rejects keys with any other input length before key codec encoding. When `fixed_value_size` is nonzero, `fce_builder_put` rejects values with any other input length before value codec encoding. `FCE_VALUE_FIXED_RECORD` requires `fixed_value_size > 0`.

### Codecs

```c
FCE_CODEC_NONE
FCE_CODEC_DELTA_I32
FCE_CODEC_RLE
FCE_CODEC_BITPACK
FCE_CODEC_USER_BYTES
FCE_CODEC_ZSTD
```

`none` and `user_bytes` are pass-through. Integer codecs require input length divisible by 4. Compressed value codecs decode into reader-owned or iterator-owned memory before returning values.

Key codec restrictions:

- Direct table keys must be pass-through so `u64` addressing is stable.
- Prefix and range lookups require pass-through key codecs so byte ordering and prefix semantics remain meaningful.
- Exact lookup backends may use compressed canonical keys.

### Flags

```c
FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE
FCE_FLAG_FORCE_HASH_COLLISION
```

- `FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE`: permits direct table creation when key space is sparse. This may allocate large slot arrays.
- `FCE_FLAG_FORCE_HASH_COLLISION`: diagnostic flag that forces all hash values to collide. It exists to verify exact-key comparison.

Runtime booleans:

- `exact_key_check`: persisted in the manifest. Exact lookups always retain key bytes for equality checks.
- `mmap_read`: when true, reader files are memory-mapped where supported. When false, files are loaded into arena-owned heap buffers.
- `read_only_after_freeze`: schema documentation flag persisted in the manifest.
- `allow_duplicate_put`: when false, duplicate keys fail freeze. When true, latest put wins for frozen backends that deduplicate; log lookup also returns the latest appended value.

### Schema Helpers

```c
FceSchema fce_schema_default(void);
FceStatus fce_schema_plan(const FcePlannerInput *input, FceSchema *out_schema);
uint64_t fce_schema_hash(const FceSchema *schema);
```

`fce_schema_default` returns a safe default:

- sorted index backend
- exact lookup
- byte keys
- variable-record values
- no codecs
- exact key check enabled
- mmap read enabled
- read-only-after-freeze enabled

`fce_schema_hash` hashes the full schema struct and is stored in `manifest.fce`. Use `fce_reader_open_expected` or `fce_validate` with an expected schema to reject stale or incompatible caches.

`fce_schema_plan` is an optional helper. It fills a normal `FceSchema` using conservative heuristics: online append selects log, prefix/sequence selects radix, dense `u64` selects direct table, large read-mostly exact sets may select MPH, and everything else falls back to sorted index.

## Memory Management API

```c
typedef struct {
    uint64_t active_allocations;
    uint64_t active_bytes;
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t peak_bytes;
} FceMemoryStats;

FceStatus fce_memory_stats(FceMemoryStats *out_stats);
void fce_free(void *ptr);
```

All library dynamic allocation goes through the unified allocator. Builder, reader, and iterator internals use arenas. File mappings and platform resources are registered as arena cleanup callbacks. `active_allocations` and `active_bytes` should return to zero after all handles and caller-owned codec buffers are closed or freed.

`fce_free(NULL)` is allowed.

## Builder API

```c
FceStatus fce_builder_open(
    const char *cache_dir,
    const FceSchema *schema,
    FceBuilder **out_builder);
```

Creates a builder and ensures `cache_dir` exists. The schema is copied. On success, `*out_builder` owns staging memory until `fce_builder_close`.

Common failures:

- invalid null arguments
- unsupported schema combination
- directory creation failure
- allocation failure

```c
FceStatus fce_builder_put(
    FceBuilder *builder,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
```

Adds one record to builder staging. The key and value are encoded through the schema codecs immediately, then copied into the builder arena. Passing `NULL` with nonzero length is invalid. Zero-length keys and values are allowed.

```c
FceStatus fce_builder_put_u64(
    FceBuilder *builder,
    uint64_t key,
    const void *value,
    size_t value_len);

FceStatus fce_builder_put_u128(
    FceBuilder *builder,
    uint64_t key_lo,
    uint64_t key_hi,
    const void *value,
    size_t value_len);
```

Convenience helpers that encode numeric keys as little-endian bytes and call `fce_builder_put`.

```c
FceStatus fce_builder_bulk_put(
    FceBuilder *builder,
    const void *const *keys,
    const size_t *key_lens,
    const void *const *values,
    const size_t *value_lens,
    size_t count);
```

Adds `count` records in order. The call stops at the first failure and returns that status.

```c
FceStatus fce_builder_freeze(FceBuilder *builder);
```

Writes backend files and `manifest.fce`. After a successful freeze, the builder is marked frozen and cannot accept more records.

Backend outputs:

- sorted: `index.fce`, `keys.fce`, `values.fce`, `manifest.fce`
- direct: dense slot `index.fce`, `keys.fce`, `values.fce`, `manifest.fce`
- radix: radix array `index.fce`, `keys.fce`, `values.fce`, `manifest.fce`
- mph: bucket/slot `index.fce`, `keys.fce`, `values.fce`, `manifest.fce`
- log: `log.fce`, `manifest.fce`

```c
FceStatus fce_builder_close(FceBuilder *builder);
```

Releases builder staging records and arena memory. Closing `NULL` is allowed and returns `FCE_OK`.

## Reader API

```c
FceStatus fce_reader_open(
    const char *cache_dir,
    FceReader **out_reader);

FceStatus fce_reader_open_expected(
    const char *cache_dir,
    const FceSchema *expected_schema,
    FceReader **out_reader);
```

Opens a cache directory, reads the manifest, restores schema flags, maps or loads backend files, validates sizes/checksums/offsets, and builds any required transient reader index. The reader holds a shared cache lock until close, so it cannot observe a half-finished freeze or log append and writers cannot replace files that the reader still maps. `fce_reader_open_expected` additionally compares `fce_schema_hash(expected_schema)` against the manifest schema hash.

Reader open validates:

- manifest magic and endian tag
- per-file sizes
- CRC64 checksums
- sorted/direct/radix/mph persisted offsets
- radix edge ordering and terminal count
- log record magic, bounds, and per-record CRC

```c
FceStatus fce_reader_get(
    FceReader *reader,
    const void *key,
    size_t key_len,
    const void **out_value,
    size_t *out_value_len);
```

Looks up one key. The query key is encoded through `schema.key_codec` when needed. Exact lookup computes backend addressing data, then compares full canonical key bytes before returning a value. The returned value pointer is valid until `fce_reader_close` unless it is returned through an iterator, in which case iterator ownership rules apply.

On miss, returns `FCE_ERR_NOT_FOUND` and leaves outputs set to null/zero.

```c
FceStatus fce_reader_get_u64(
    FceReader *reader,
    uint64_t key,
    const void **out_value,
    size_t *out_value_len);

FceStatus fce_reader_get_u128(
    FceReader *reader,
    uint64_t key_lo,
    uint64_t key_hi,
    const void **out_value,
    size_t *out_value_len);
```

Numeric-key convenience helpers using the same little-endian encoding as the builder helpers.

```c
FceStatus fce_reader_get_copy(
    FceReader *reader,
    const void *key,
    size_t key_len,
    void *out_value,
    size_t *inout_value_len);
```

Copies a value into caller memory. If `out_value` is null or too small, returns `FCE_ERR_INVALID_ARGUMENT` and writes the required value length to `*inout_value_len`.

```c
FceStatus fce_reader_get_batch(
    FceReader *reader,
    const void *const *keys,
    const size_t *key_lens,
    const void **out_values,
    size_t *out_value_lens,
    FceStatus *out_statuses,
    size_t count);
```

Runs multiple independent lookups. The function returns `FCE_OK` when the batch arguments are valid. Per-key hit/miss/error statuses are stored in `out_statuses`.

```c
void fce_reader_close(FceReader *reader);
```

Releases reader arenas, heap-loaded files, transient log index, and mmap cleanup callbacks. Closing `NULL` is allowed.

## Iterator And Scan API

```c
FceStatus fce_reader_prefix_scan(
    FceReader *reader,
    const void *prefix,
    size_t prefix_len,
    FceIterator **out_iterator);
```

Creates an iterator over keys beginning with `prefix`. Supported on sorted index and radix. Prefix scan requires pass-through key codecs.

```c
FceStatus fce_reader_range_scan(
    FceReader *reader,
    const void *start_key,
    size_t start_key_len,
    const void *end_key,
    size_t end_key_len,
    FceIterator **out_iterator);
```

Creates an iterator over canonical byte keys in the half-open interval `[start_key, end_key)`. Supported on sorted index and radix. Range scan requires pass-through key codecs.

```c
FceStatus fce_reader_scan_all(
    FceReader *reader,
    FceIterator **out_iterator);
```

Creates an iterator over all records for sorted, radix, direct, and mph caches. Log caches should be compacted before scan-all when latest-value export semantics are required.

```c
FceStatus fce_iterator_next(
    FceIterator *it,
    const void **out_key,
    size_t *out_key_len,
    const void **out_value,
    size_t *out_value_len);
```

Returns the next key/value pair. Returns `FCE_ERR_NOT_FOUND` when exhausted. Key pointers are reader-owned. Values are reader-owned for pass-through codecs and iterator-owned for decoded compressed values.

```c
void fce_iterator_close(FceIterator *it);
```

Releases iterator match arrays and decoded iterator values. Closing `NULL` is allowed.

## Log And Compaction

```c
FceStatus fce_log_append(
    const char *cache_dir,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
```

Appends one record to an existing log backend. The call encodes key/value using the schema stored in the manifest, appends a CRC-protected record, updates record count, log size, log checksum, and rewrites the manifest. Appends are serialized with an internal cache lock, so multiple processes may append to the same log cache without racing the manifest update.

```c
FceStatus fce_compact(
    const char *source_cache_dir,
    FceBackendKind backend,
    const char *output_cache_dir);
```

Reads a source cache and writes a new cache using `backend`. Log compaction preserves latest-value semantics for duplicate keys. Non-log caches are exported through scan-all.

## Inspection And Validation

```c
FceStatus fce_inspect(const char *cache_dir, FceManifestInfo *out_info);
```

Reads `manifest.fce` into `out_info`. It validates manifest magic and endian tag but does not validate all backend files.

```c
FceStatus fce_validate(const char *cache_dir, const FceSchema *expected_schema);
```

Fully opens and validates a cache, then closes it. Use this for CI checks or cache startup checks.

## Codec API

```c
FceStatus fce_codec_encode(
    FceCodecKind codec,
    const void *input,
    size_t input_len,
    void **out_data,
    size_t *out_len);

FceStatus fce_codec_decode(
    FceCodecKind codec,
    const void *input,
    size_t input_len,
    void **out_data,
    size_t *out_len);
```

The output buffer is caller-owned and must be freed with `fce_free`. On failure, no output buffer is returned.

Codec-specific behavior:

- `none`: byte-for-byte copy.
- `user_bytes`: byte-for-byte copy for application-defined encodings.
- `delta_i32`: int32 array; stores first value and signed deltas; rejects overflow.
- `rle`: int32 run-length encoding; stores original count and `(value, count)` pairs.
- `bitpack`: uint32 array; stores element count, bit width, and packed payload.
- `zstd`: optional zstd frame compression. It is available only when Fast Cache Engine is built with libzstd.

```c
int fce_codec_available(FceCodecKind codec);
```

Use this to detect optional codecs at runtime. Built-in codecs return 1. `FCE_CODEC_ZSTD` returns 1 only when libzstd was found at build time.

## Manifest Info

```c
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
```

`backend_meta0` stores backend-specific numeric metadata such as direct slot count, radix node count, or mph bucket count. `backend_meta1` stores user flags and persisted runtime schema booleans. The fixed-size schema fields are persisted explicitly so `fce_reader_open` and `fce_compact` keep fixed-record constraints after reopening a cache. Unsupported manifest, schema, or algorithm versions are rejected during open/validate.

## Python API

```python
from fast_cache_engine import CacheBuilder, CacheReader, CacheSchema
```

### CacheSchema

```python
schema = CacheSchema(
    backend="sorted_index",
    lookup="exact",
    key_kind="bytes",
    value_kind="var_record",
    key_codec="none",
    value_codec="none",
    mmap_read=True,
    exact_key_check=True,
    allow_duplicate_put=False,
    fixed_key_size=0,
    fixed_value_size=0,
)
```

Python enum strings map directly to C enum values. Unknown strings raise `ValueError`.

### CacheBuilder

```python
with CacheBuilder("cache", schema) as builder:
    builder.put(b"key", b"value")
    builder.put_u64(42, b"value")
    builder.freeze()
```

`put` accepts bytes-like objects. `freeze` must be called before the cache is complete.

### CacheReader

```python
with CacheReader("cache") as reader:
    value = reader.get(b"key")
    value_u64 = reader.get_u64(42)
    rows = reader.get_many([b"k1", b"k2"])
    for key, value in reader.prefix_scan(b"user:"):
        ...
```

`get` and `get_u64` return `memoryview | None`. Convert to `bytes` if the value must outlive the reader.

Additional helpers:

```python
log_append("log_cache", b"key", b"value")
compact("log_cache", "sorted_index", "compact_cache")
export_sqlite("cache", "cache.sqlite")
import_sqlite("cache.sqlite", "cache_from_sqlite", CacheSchema(backend="mph"))
export_lmdb("cache", "cache.lmdb")
import_lmdb("cache.lmdb", "cache_from_lmdb", CacheSchema(backend="mph"))
abi_version()
version_string()
codec_available("zstd")
plan_schema(PlannerInput(key_kind="u64", estimated_record_count=1000, dense_u64_max_key=1200))
```

The SQLite bridge uses a table with `key BLOB PRIMARY KEY` and `value BLOB NOT NULL` by default.

The LMDB bridge requires the optional Python package `lmdb`. If it is not installed, `export_lmdb` and `import_lmdb` raise `ImportError` with a clear message.

## CLI

```text
fce build --schema schema.json --input records.fce --output cache_dir
fce inspect cache_dir
fce validate cache_dir
fce dump cache_dir --limit 10
fce compact log_cache_dir --backend sorted_index --output output_cache_dir
```

`compact` also accepts the legacy positional form:
`fce compact log_cache_dir sorted_index output_cache_dir`.

`records.fce` format:

```text
uint64 key_len
uint64 value_len
key bytes
value bytes
...
```

`schema.json` supports string enum fields and boolean flags:

```json
{
  "backend": "sorted_index",
  "lookup": "exact",
  "key_kind": "bytes",
  "value_kind": "var_record",
  "key_codec": "none",
  "value_codec": "none",
  "fixed_key_size": 0,
  "fixed_value_size": 0,
  "direct_table_key_bits": 0,
  "mmap_read": true,
  "exact_key_check": true,
  "allow_duplicate_put": false,
  "allow_sparse_direct_table": false
}
```

Unknown enum values and invalid boolean syntax fail instead of silently falling back to defaults.
