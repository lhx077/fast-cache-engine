# Backends

Fast Cache Engine provides five storage backends behind the same builder and reader APIs. All exact lookup backends preserve canonical key bytes and verify exact key equality before returning a value.

## Selection Guide

| Backend | Best For | Mutable After Freeze | Main Lookup Cost | Scan Support |
| --- | --- | --- | --- | --- |
| `sorted_index` | General exact lookup, stable default | No | `O(log N + collisions)` | prefix, range, all |
| `direct_table` | Dense `u64` keys | No | `O(1)` array slot | all |
| `mph` | High-frequency read-only exact lookup | No | `O(1)` bucket/slot | all |
| `radix` | Prefix/range byte-key queries | No | `O(key_len)` traversal | prefix, range, all |
| `log` | Online append and ingestion | Yes, append-only | `O(log N + duplicate scan)` | compact first |

## Sorted Index

Sorted index is the default backend. It works for arbitrary byte keys and is the most conservative choice.

Build flow:

```text
put records
encode keys/values
deduplicate if enabled
write keys.fce and values.fce
compute hash128 for each canonical key
sort index entries by hash_hi, hash_lo, key_len, key_offset
write index.fce and manifest.fce
```

Lookup flow:

```text
encode query key if needed
compute hash128
binary search sorted index by hash
scan equal-hash range
compare full canonical key bytes
return value slice
```

Strengths:

- Handles arbitrary byte keys.
- mmap-friendly contiguous index.
- Correct under hash collisions.
- Supports exact lookup, prefix scan, range scan, and scan-all.

Tradeoffs:

- Lookup has binary-search cost.
- Prefix/range scan currently walks index entries and checks key bytes; radix is better for large prefix-heavy workloads.

## Direct Table

Direct table maps dense `u64` keys directly to slots.

Build requirements:

- `schema.key_kind` must be `FCE_KEY_U64`.
- Canonical keys must be exactly 8 bytes.
- Sparse keyspaces are rejected unless `FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE` is set.

Lookup flow:

```text
slot = slots[key]
if slot.present and slot.key_u64 == key:
    return value slice
else:
    miss
```

Strengths:

- Very small lookup constant.
- Good for compact dense ID spaces.
- mmap-friendly slot array.

Tradeoffs:

- Sparse keyspace can allocate large files.
- Only supports `u64` addressing.

## MPH

The MPH backend is a read-only exact lookup backend with one persisted slot per record. The implementation stores one seed per bucket and searches for seeds that place all bucket items into unused global slots.

Build flow:

```text
deduplicate records
write keys.fce and values.fce
group keys by hash bucket
process largest buckets first
find seed for each bucket
write bucket seeds and slots to index.fce
```

Lookup flow:

```text
hash query key
bucket = hash_hi % bucket_count
seed = buckets[bucket].seed
slot = fnv1a64(key, seed) % slot_count
check slot.present
check stored hash
compare exact canonical key bytes
return value slice
```

Strengths:

- `O(1)` lookup path.
- Exactly `record_count` value slots.
- Unknown keys cannot false-hit because exact key bytes are checked.

Tradeoffs:

- Build can fail with `FCE_ERR_UNSUPPORTED` if seed search cannot place a bucket within the configured search bound.
- Best for stable read-only key sets.

## Compact Radix

Radix stores byte-key prefixes as mmap-safe arrays:

- header
- node array
- sorted edge array
- terminal value metadata array

No native pointers are persisted. Nodes reference children and values by array index.

Lookup flow:

```text
node = root
for byte in key:
    binary search sorted child edges
    node = child
check terminal value index
compare exact key bytes
return value slice
```

Prefix scan:

```text
walk prefix bytes to prefix node
depth-first traverse descendants
return terminal values
```

Strengths:

- Natural prefix scan.
- Supports exact lookup and range scan.
- Persisted structure is portable and mmap-safe.

Tradeoffs:

- Larger and slower than direct table for dense integer keys.
- Not ideal for random keys without shared prefixes.

## Append Log

Log backend supports online append. It stores records in `log.fce`:

```text
uint64 magic
uint64 key_len
uint64 value_len
key bytes
value bytes
uint64 record_crc64
```

Reader open validates every record and builds a transient hash-sorted index in memory. Lookup returns the latest matching record when duplicate keys exist.

Strengths:

- Supports incremental writes through `fce_log_append`.
- Simple ingestion path.
- Can be compacted into sorted, direct, radix, or mph backend.

Tradeoffs:

- Reader open cost includes full log validation and transient index build.
- File can grow with obsolete duplicate values.
- Use `fce_compact` for long-lived read-heavy caches.
