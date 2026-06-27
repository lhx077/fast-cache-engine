# Fast Cache Engine API 参考

本文说明 Fast Cache Engine 的 C ABI、Python 绑定、CLI、内存所有权、错误行为和 schema 语义。

公共声明位于 `include/`。主头文件是 `fce_cache.h`；`fce_builder.h`、`fce_reader.h`、`fce_schema.h`、`fce_error.h` 是按主题拆分的便捷入口。arena API 是内部实现细节，不作为 public header 安装。

## 内存所有权模型

Fast Cache Engine 有三类内存所有权：

- Reader 持有的 view：`fce_reader_get` 返回的 value、iterator 返回的 key/value 通常指向 mmap、reader arena 或 iterator arena。它们只在对应 reader 或 iterator 关闭前有效。
- 调用者持有的 buffer：`fce_codec_encode` 和 `fce_codec_decode` 返回的 buffer 必须用 `fce_free` 释放。
- 不透明句柄：`FceBuilder`、`FceReader`、`FceIterator` 必须分别用 `fce_builder_close`、`fce_reader_close`、`fce_iterator_close` 释放。

不要对 Fast Cache Engine 返回的内存调用系统 `free`。只有明确写明由调用者持有的 buffer 才用 `fce_free` 释放。

## 线程和进程安全

所有会访问 cache 目录的操作都会通过内部 `cache.lock` 文件串行化。`fce_builder_freeze` 和 `fce_log_append` 这类 writer 使用独占锁。`fce_reader_open` 和 `fce_reader_open_expected` 打开的 reader 会持有共享锁直到 `fce_reader_close`；`fce_validate` 和 `fce_inspect` 也使用共享锁。这能防止 reader 看到写到一半的快照，也能防止 writer 覆盖仍被 mmap reader 使用的文件。

已经打开的 reader 是一个快照视图。它不会看到之后的 append 或 freeze；如果需要读取新记录，需要重新打开 reader。写同一个 cache 目录的操作会等待已打开 reader 关闭。内部 allocator、内存统计和 arena bookkeeping 已经做并发保护。不要在另一个线程仍使用同一 builder、reader 或 iterator 时关闭该句柄。同一个 builder 或 iterator 上的可变操作仍应由调用方自行串行化。

## 错误码

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

- `FCE_OK`：操作成功。
- `FCE_ERR_INVALID_ARGUMENT`：空指针、长度非法、所选 backend 不支持的 key 形状、不允许重复 key、copy buffer 太小，或 schema/backend 组合非法。
- `FCE_ERR_IO`：文件系统读、写、打开或关闭失败。
- `FCE_ERR_CORRUPT`：持久化文件的 magic、size、checksum、offset 或 record 校验失败。
- `FCE_ERR_NOT_FOUND`：查询 miss，或 iterator 已到尾。
- `FCE_ERR_UNSUPPORTED`：选择了当前 build 不支持的功能组合。
- `FCE_ERR_SCHEMA_MISMATCH`：调用方提供的 expected schema hash 与 manifest 不一致。
- `FCE_ERR_OUT_OF_MEMORY`：分配失败，或请求的结构大小发生溢出。

```c
const char *fce_status_string(FceStatus status);
uint32_t fce_abi_version(void);
const char *fce_version_string(void);
```

`fce_status_string` 返回静态英文诊断字符串，返回指针不需要释放。`fce_abi_version` 返回 public header 中的 packed `FCE_ABI_VERSION`。`fce_version_string` 返回库版本字符串。动态加载 shared library 的调用方应先检查 ABI/version，再绑定可选符号。

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

- `sorted_index`：默认 exact lookup backend。保存排序 hash entry 和完整 key bytes。
- `mph`：面向高频只读 exact lookup，使用 bucket seed 和固定 slot。
- `direct_table`：稠密 `u64` 直接寻址，要求 `key_kind = FCE_KEY_U64`。
- `radix`：面向 prefix/range byte-key 查询，持久化结构是 mmap-safe array。
- `log`：append-only 在线写入 backend。reader open 时构建临时内存索引。

### Lookup Kind

```c
FCE_LOOKUP_EXACT
FCE_LOOKUP_PREFIX
FCE_LOOKUP_RANGE
```

lookup kind 记录预期查询方式，并用于验证部分组合。exact lookup 支持所有 backend。prefix scan 支持 sorted 和 radix。range scan 支持 sorted 和 radix，按 canonical byte ordering 处理。

### Key Kind

```c
FCE_KEY_BYTES
FCE_KEY_U64
FCE_KEY_U128
FCE_KEY_INT_TUPLE
FCE_KEY_SEQUENCE
```

`FCE_KEY_BYTES` 是通用选项。`FCE_KEY_U64` 启用 `fce_builder_put_u64`、`fce_reader_get_u64` 和 direct table。`FCE_KEY_U128` 对应两个 little-endian `uint64_t` word 的 helper API。tuple 和 sequence 是 schema 层对应用编码的说明，底层仍保存 canonical bytes。

### Value Kind

```c
FCE_VALUE_BYTES
FCE_VALUE_FIXED_RECORD
FCE_VALUE_VAR_RECORD
```

库只保存 value bytes。fixed/variable record kind 用于说明应用层布局。当不需要解码时，fixed-size value 可以作为直接 view 返回。

`fixed_key_size` 非 0 时，`fce_builder_put` 会在 key codec 编码前拒绝长度不一致的 key。`fixed_value_size` 非 0 时，会在 value codec 编码前拒绝长度不一致的 value。`FCE_VALUE_FIXED_RECORD` 要求 `fixed_value_size > 0`。

### Codecs

```c
FCE_CODEC_NONE
FCE_CODEC_DELTA_I32
FCE_CODEC_RLE
FCE_CODEC_BITPACK
FCE_CODEC_USER_BYTES
FCE_CODEC_ZSTD
```

`none` 和 `user_bytes` 是 pass-through。整数 codec 要求输入长度能被 4 整除。压缩 value codec 在返回 value 前会解码到 reader 或 iterator 持有的内存。

Key codec 限制：

- Direct table key 必须 pass-through，才能保证 `u64` 地址稳定。
- Prefix/range lookup 必须 pass-through，才能保持字节序和前缀语义。
- Exact lookup backend 可以使用压缩 canonical key。

### Flags

```c
FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE
FCE_FLAG_FORCE_HASH_COLLISION
```

- `FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE`：允许 key space 稀疏时仍创建 direct table，可能生成很大的 slot array。
- `FCE_FLAG_FORCE_HASH_COLLISION`：诊断 flag，强制所有 hash 碰撞，用于验证 exact-key comparison。

运行时布尔字段：

- `exact_key_check`：写入 manifest。exact lookup 始终保留 key bytes 并做最终相等性比较。
- `mmap_read`：为 true 时 reader 优先 mmap；为 false 时文件读入 arena-owned heap buffer。
- `read_only_after_freeze`：schema 文档标志，写入 manifest。
- `allow_duplicate_put`：false 时重复 key 在 freeze 阶段报错；true 时 frozen backend 去重后最新值胜出，log lookup 也返回最新 append 的值。

### Schema Helper

```c
FceSchema fce_schema_default(void);
FceStatus fce_schema_plan(const FcePlannerInput *input, FceSchema *out_schema);
uint64_t fce_schema_hash(const FceSchema *schema);
```

`fce_schema_default` 返回安全默认值：sorted index、exact lookup、bytes key、var-record value、无 codec、启用 exact key check、启用 mmap read、freeze 后只读。

`fce_schema_hash` 对完整 schema 结构求 hash，并写入 `manifest.fce`。使用 `fce_reader_open_expected` 或带 expected schema 的 `fce_validate` 可以拒绝过期或不兼容 cache。

`fce_schema_plan` 是可选 helper。它用保守规则填出普通 `FceSchema`：online append 选 log，prefix/sequence 选 radix，稠密 `u64` 选 direct table，大型 read-mostly exact set 可选 MPH，其余回退到 sorted index。

## 内存管理 API

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

库内部所有动态分配都走统一 allocator。builder、reader、iterator 内部对象使用 arena。文件映射和平台资源通过 arena cleanup callback 注册。所有 handle 关闭、所有 caller-owned codec buffer 释放后，`active_allocations` 和 `active_bytes` 应回到 0。

`fce_free(NULL)` 是允许的。

## Builder API

```c
FceStatus fce_builder_open(
    const char *cache_dir,
    const FceSchema *schema,
    FceBuilder **out_builder);
```

创建 builder 并确保 `cache_dir` 存在。schema 会复制到 builder 内部。成功后，`*out_builder` 持有 staging memory，直到 `fce_builder_close`。

常见失败原因：

- 参数为空
- schema 组合不支持
- 目录创建失败
- 内存分配失败

```c
FceStatus fce_builder_put(
    FceBuilder *builder,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
```

向 staging 区追加一条记录。key/value 会立即按 schema codec 编码，然后复制到 builder arena。`NULL + 非 0 长度` 非法；0 长度 key/value 允许。

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

数字 key helper 会按 little-endian bytes 编码，然后调用 `fce_builder_put`。

```c
FceStatus fce_builder_bulk_put(
    FceBuilder *builder,
    const void *const *keys,
    const size_t *key_lens,
    const void *const *values,
    const size_t *value_lens,
    size_t count);
```

按顺序写入 `count` 条记录。遇到第一处失败时停止并返回该 status。

```c
FceStatus fce_builder_freeze(FceBuilder *builder);
```

写出 backend 文件和 `manifest.fce`。freeze 成功后 builder 被标记为 frozen，不能再 put。

backend 输出：

- sorted：`index.fce`、`keys.fce`、`values.fce`、`manifest.fce`
- direct：dense slot `index.fce`、`keys.fce`、`values.fce`、`manifest.fce`
- radix：radix array `index.fce`、`keys.fce`、`values.fce`、`manifest.fce`
- mph：bucket/slot `index.fce`、`keys.fce`、`values.fce`、`manifest.fce`
- log：`log.fce`、`manifest.fce`

```c
FceStatus fce_builder_close(FceBuilder *builder);
```

释放 builder staging records 和 arena memory。关闭 `NULL` 是允许的，并返回 `FCE_OK`。

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

打开 cache 目录，读取 manifest，恢复 schema flags，mmap 或加载 backend 文件，校验 size/checksum/offset，并构建必要的 transient reader index。reader 会持有共享 cache lock 直到 close，因此不会看到只写完一半的 freeze 或 log append，writer 也不会覆盖 reader 仍在 mmap 的文件。`fce_reader_open_expected` 会额外比较 `fce_schema_hash(expected_schema)` 与 manifest schema hash。

reader open 会校验：

- manifest magic 和 endian tag
- 每个文件的 size
- CRC64 checksum
- sorted/direct/radix/mph 持久化 offset
- radix edge ordering 和 terminal count
- log record magic、bounds 和 per-record CRC

```c
FceStatus fce_reader_get(
    FceReader *reader,
    const void *key,
    size_t key_len,
    const void **out_value,
    size_t *out_value_len);
```

查询单个 key。必要时先按 `schema.key_codec` 编码查询 key。exact lookup 会计算 backend 定位数据，然后比较完整 canonical key bytes，最后返回 value。返回 value pointer 在 `fce_reader_close` 前有效；如果 value 来自 iterator，则遵守 iterator 生命周期。

miss 时返回 `FCE_ERR_NOT_FOUND`，并把输出置为 null/zero。

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

数字 key helper 与 builder helper 使用相同 little-endian 编码。

```c
FceStatus fce_reader_get_copy(
    FceReader *reader,
    const void *key,
    size_t key_len,
    void *out_value,
    size_t *inout_value_len);
```

把 value 复制到调用方 buffer。如果 `out_value` 为空或 buffer 太小，返回 `FCE_ERR_INVALID_ARGUMENT`，并把所需长度写入 `*inout_value_len`。

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

执行多个独立 lookup。函数返回值表示 batch 参数本身是否有效；每个 key 的 hit/miss/error 写入 `out_statuses`。

```c
void fce_reader_close(FceReader *reader);
```

释放 reader arena、heap-loaded file、transient log index 和 mmap cleanup callback。关闭 `NULL` 是允许的。

## Iterator And Scan API

```c
FceStatus fce_reader_prefix_scan(
    FceReader *reader,
    const void *prefix,
    size_t prefix_len,
    FceIterator **out_iterator);
```

创建一个遍历指定 prefix 的 iterator。支持 sorted index 和 radix。prefix scan 要求 key codec 为 pass-through。

```c
FceStatus fce_reader_range_scan(
    FceReader *reader,
    const void *start_key,
    size_t start_key_len,
    const void *end_key,
    size_t end_key_len,
    FceIterator **out_iterator);
```

创建一个遍历 canonical byte key 半开区间 `[start_key, end_key)` 的 iterator。支持 sorted index 和 radix。range scan 要求 key codec 为 pass-through。

```c
FceStatus fce_reader_scan_all(
    FceReader *reader,
    FceIterator **out_iterator);
```

为 sorted、radix、direct、mph cache 创建全量 iterator。log cache 如果需要 latest-value export 语义，建议先 compact。

```c
FceStatus fce_iterator_next(
    FceIterator *it,
    const void **out_key,
    size_t *out_key_len,
    const void **out_value,
    size_t *out_value_len);
```

返回下一组 key/value。到尾时返回 `FCE_ERR_NOT_FOUND`。key pointer 归 reader 所有。pass-through codec 的 value 归 reader 所有；压缩 value 会解码到 iterator 持有的内存。

```c
void fce_iterator_close(FceIterator *it);
```

释放 iterator match array 和 iterator 持有的 decoded value。关闭 `NULL` 是允许的。

## Log And Compaction

```c
FceStatus fce_log_append(
    const char *cache_dir,
    const void *key,
    size_t key_len,
    const void *value,
    size_t value_len);
```

向已有 log backend 追加一条记录。调用会读取 manifest 中的 schema，对 key/value 编码，追加带 CRC 的 record，更新 record count、log size、log checksum，并重写 manifest。追加操作会通过内部 cache lock 串行化，所以多个进程可以同时向同一个 log cache append，不会竞争 manifest 更新。

```c
FceStatus fce_compact(
    const char *source_cache_dir,
    FceBackendKind backend,
    const char *output_cache_dir);
```

读取 source cache，并用目标 `backend` 写出新的 cache。log compaction 保留 duplicate key 的 latest-value 语义。非 log cache 通过 scan-all 导出。

## Inspect And Validate

```c
FceStatus fce_inspect(const char *cache_dir, FceManifestInfo *out_info);
```

读取 `manifest.fce` 到 `out_info`。它校验 manifest magic 和 endian tag，但不会完整校验所有 backend 文件。

```c
FceStatus fce_validate(const char *cache_dir, const FceSchema *expected_schema);
```

完整打开并校验 cache，然后关闭。适合 CI 或应用启动时的 cache health check。

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

输出 buffer 由调用方持有，必须用 `fce_free` 释放。失败时不会返回输出 buffer。

codec 行为：

- `none`：逐字节复制。
- `user_bytes`：应用自定义 encoding 的逐字节复制。
- `delta_i32`：int32 array，保存首值和 signed delta，拒绝溢出。
- `rle`：int32 run-length encoding，保存原始数量和 `(value, count)` pair。
- `bitpack`：uint32 array，保存元素数量、bit width 和 packed payload。
- `zstd`：可选 zstd frame compression。只有构建时启用 libzstd 才可用。

```c
int fce_codec_available(FceCodecKind codec);
```

用于运行时检测 optional codec。内置 codec 返回 1。`FCE_CODEC_ZSTD` 只有构建时找到 libzstd 才返回 1。

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

`backend_meta0` 保存 backend-specific 数字元数据，例如 direct slot count、radix node count 或 mph bucket count。`backend_meta1` 保存 user flags 和 runtime schema booleans。fixed-size schema 字段会显式持久化，因此 `fce_reader_open` 和 `fce_compact` 重新打开 cache 后仍保留 fixed-record 约束。不支持的 manifest、schema 或 algorithm version 会在 open/validate 时被拒绝。

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

Python enum string 直接映射到 C enum。未知字符串会抛出 `ValueError`。

### CacheBuilder

```python
with CacheBuilder("cache", schema) as builder:
    builder.put(b"key", b"value")
    builder.put_u64(42, b"value")
    builder.freeze()
```

`put` 接受 bytes-like object。cache 完成前必须调用 `freeze`。

### CacheReader

```python
with CacheReader("cache") as reader:
    value = reader.get(b"key")
    value_u64 = reader.get_u64(42)
    rows = reader.get_many([b"k1", b"k2"])
    for key, value in reader.prefix_scan(b"user:"):
        ...
```

`get` 和 `get_u64` 返回 `memoryview | None`。如果 value 需要脱离 reader 生命周期，转换为 `bytes`。

其他 helper：

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

SQLite bridge 默认使用 `key BLOB PRIMARY KEY` 和 `value BLOB NOT NULL` 表。

LMDB bridge 需要可选 Python 包 `lmdb`。未安装时，`export_lmdb` 和 `import_lmdb` 会抛出明确的 `ImportError`。

## CLI

```text
fce build --schema schema.json --input records.fce --output cache_dir
fce inspect cache_dir
fce validate cache_dir
fce dump cache_dir --limit 10
fce compact log_cache_dir --backend sorted_index --output output_cache_dir
```

`compact` 也接受旧的位置参数形式：

```text
fce compact log_cache_dir sorted_index output_cache_dir
```

`records.fce` 格式：

```text
uint64 key_len
uint64 value_len
key bytes
value bytes
...
```

`schema.json` 支持字符串 enum 和布尔 flag：

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

未知 enum value 和非法 boolean syntax 会失败，不会静默回退到默认值。
