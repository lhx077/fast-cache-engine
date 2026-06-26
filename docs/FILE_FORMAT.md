# File Format

A cache is a directory containing a manifest and backend data files.

```text
manifest.fce
index.fce
keys.fce
values.fce
log.fce
```

Not every backend uses every file. Missing unused files are treated as zero-sized.

## Endianness

The current format is little-endian/native for fixed-width integer structs and stores an endian tag in the manifest:

```text
0x0102030405060708
```

Readers reject caches whose endian tag does not match.

## Manifest

`manifest.fce` is a fixed-size `FceManifestInfo` structure. `key_codec` and `value_codec` store the numeric `FceCodecKind`, including optional values such as `FCE_CODEC_ZSTD`; a reader that does not support the requested codec rejects decode operations with `FCE_ERR_UNSUPPORTED`.

```c
typedef struct {
    char magic[8];              /* "FCEv0001" */
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

Readers reject manifests whose manifest, schema, or algorithm version is not supported by this build. `fixed_key_size`, `fixed_value_size`, and `direct_table_key_bits` persist the corresponding schema fields so reopened caches and compaction keep fixed-record constraints intact.

`backend_meta1` persists user flags and runtime schema booleans:

- exact key check
- mmap read
- read-only after freeze
- duplicate policy

## `keys.fce`

Stores canonical encoded key bytes contiguously:

```text
key0 bytes
key1 bytes
...
```

Exact lookup backends keep key bytes even when hashing is used. Hashes only locate candidate entries; final equality uses full key bytes.

## `values.fce`

Stores canonical encoded value bytes contiguously:

```text
value0 bytes
value1 bytes
...
```

With pass-through value codecs, readers return views directly into this file mapping or heap-loaded buffer. With compressed value codecs, readers decode into owned memory before returning a value.

## Sorted Index `index.fce`

Sorted index entries are:

```c
typedef struct {
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t reserved0;
    uint64_t value_offset;
    uint32_t value_len;
    uint32_t flags;
} SortedEntry;
```

Entries are sorted by:

```text
hash_hi, hash_lo, key_len, key_offset
```

Reader validation checks that every key and value offset/length stays inside `keys.fce` and `values.fce`.

## Direct Table `index.fce`

Direct table stores an array of slots addressed by `u64` key:

```c
typedef struct {
    uint8_t present;
    uint8_t reserved[7];
    uint64_t key_u64;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t value_len;
    uint64_t value_offset;
} DirectSlot;
```

`backend_meta0` stores slot count.

## MPH `index.fce`

MPH index layout:

```text
MphHeader
MphBucket[bucket_count]
MphSlot[slot_count]
```

The header stores offsets to bucket and slot arrays. `bucket_count` and `slot_count` both equal `record_count` in the current format.

Reader validation checks:

- magic
- offsets
- array sizes
- present slot count
- key/value ranges

## Radix `index.fce`

Radix index layout:

```text
RadixHeader
RadixNode[node_count]
RadixEdge[edge_count]
RadixValue[value_count]
```

Nodes store their first edge index, edge count, and terminal value index. Edges are sorted by byte label within each node. Values store key/value offsets and lengths.

Reader validation checks:

- magic
- offsets and exact section boundaries
- child indexes
- sorted edges
- terminal count
- key/value ranges

## Log `log.fce`

Append log records are stored sequentially:

```text
uint64 magic
uint64 key_len
uint64 value_len
key bytes
value bytes
uint64 record_crc64
```

The record CRC covers the header, key bytes, and value bytes. The manifest also stores a whole-file CRC64 for `log.fce`.

Reader open validates every record before serving lookups.

## Validation Rules

Reader open rejects a cache when any of these fail:

- manifest magic
- endian tag
- declared file sizes
- per-file CRC64
- backend magic
- section offset arithmetic
- key/value offset bounds
- log record CRC
- schema hash when an expected schema is supplied
