# 文件格式

一个 cache 是一个目录，包含 manifest 和 backend 数据文件：

```text
manifest.fce
index.fce
keys.fce
values.fce
log.fce
```

不是所有 backend 都使用所有文件；未使用文件按 0 大小处理。

## 字节序

当前格式对固定宽度整数使用 little-endian/native 表示，并在 manifest 中保存 endian tag：

```text
0x0102030405060708
```

reader 会拒绝 endian tag 不匹配的 cache。

## Manifest

`manifest.fce` 是固定大小的 `FceManifestInfo` 结构。`key_codec` 和 `value_codec` 保存 numeric `FceCodecKind`，包括 `FCE_CODEC_ZSTD` 这类 optional value；reader 如果不支持请求的 codec，会在需要解码时返回 `FCE_ERR_UNSUPPORTED`。

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

reader 会拒绝当前 build 不支持的 manifest、schema 或 algorithm version。`fixed_key_size`、`fixed_value_size` 和 `direct_table_key_bits` 会持久化对应 schema 字段，确保 reopen 和 compaction 不丢失 fixed-record 约束。

`backend_meta1` 持久化 user flags 和 runtime schema booleans：

- exact key check
- mmap read
- read-only after freeze
- duplicate policy

## `keys.fce`

连续保存 canonical encoded key bytes：

```text
key0 bytes
key1 bytes
...
```

Exact lookup backend 即使使用 hash，也保留完整 key bytes。hash 只用于定位候选 entry，最终相等性由完整 key bytes 比较决定。

## `values.fce`

连续保存 canonical encoded value bytes：

```text
value0 bytes
value1 bytes
...
```

pass-through value codec 可以直接返回指向文件映射或 heap-loaded buffer 的 view。压缩 value codec 会在返回前解码到 owned memory。

## Sorted Index `index.fce`

Sorted index entry：

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

排序键：

```text
hash_hi, hash_lo, key_len, key_offset
```

reader validation 会检查每个 key/value offset 和 length 都落在 `keys.fce`、`values.fce` 范围内。

## Direct Table `index.fce`

Direct table 保存由 `u64` key 寻址的 slot array：

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

`backend_meta0` 保存 slot count。

## MPH `index.fce`

MPH index layout：

```text
MphHeader
MphBucket[bucket_count]
MphSlot[slot_count]
```

header 保存 bucket array 和 slot array 的 offset。当前格式中，`bucket_count` 和 `slot_count` 都等于 `record_count`。

reader validation 会检查：

- magic
- offsets
- array sizes
- present slot count
- key/value ranges

## Radix `index.fce`

Radix index layout：

```text
RadixHeader
RadixNode[node_count]
RadixEdge[edge_count]
RadixValue[value_count]
```

node 保存 first edge index、edge count 和 terminal value index。edge 在每个 node 内按 byte label 排序。value 保存 key/value offset 和 length。

reader validation 会检查：

- magic
- offsets 和 section boundaries
- child indexes
- sorted edges
- terminal count
- key/value ranges

## Log `log.fce`

Append log record 顺序写入：

```text
uint64 magic
uint64 key_len
uint64 value_len
key bytes
value bytes
uint64 record_crc64
```

record CRC 覆盖 header、key bytes 和 value bytes。manifest 还保存整个 `log.fce` 的 CRC64。

reader open 会先校验所有 record，再提供 lookup。

## Reader 校验规则

reader open 会在任一条件失败时拒绝 cache：

- manifest magic
- endian tag
- 声明文件大小
- per-file CRC64
- backend magic
- section offset arithmetic
- key/value offset bounds
- log record CRC
- supplied expected schema hash
