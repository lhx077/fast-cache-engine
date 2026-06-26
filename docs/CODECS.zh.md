# 编码器

Codec 在持久化前或读取后转换 byte buffer。Fast Cache Engine 不从 bytes 猜测业务类型；被选择的 codec 决定这些 bytes 如何解释。

## 所有权

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

两个函数都会分配调用者持有的输出 buffer。必须使用 `fce_free` 释放。

Builder API 会在写入前按 schema 编码 key 和 value。Reader API 在 `schema.value_codec` 为压缩 codec 时解码 value。查询 key 在 backend 支持时也会先按 `schema.key_codec` 编码。

## `none`

逐字节复制。

适合：

- value 已经是最终二进制格式。
- 读路径需要 mmap zero-copy。
- key bytes 必须保留 lexical prefix/range 语义。

## `user_bytes`

用于应用自定义 encoding 的逐字节复制。

适合应用层已经拥有自己的序列化格式时使用。Fast Cache Engine 把这些 bytes 视为 opaque payload，只负责保存、校验和返回。

## `delta_i32`

输入是 little-endian/native `int32_t` 数组。编码输出保存：

```text
first value
delta(value[1] - value[0])
delta(value[2] - value[1])
...
```

校验规则：

- input length 必须能被 4 整除。
- encode 拒绝 signed 32-bit delta overflow。
- decode 拒绝重建值的 signed 32-bit overflow。

适合单调或变化平滑的整数数组。

## `rle`

输入是 `int32_t` 数组。编码输出保存：

```text
int32 original_count
int32 value_0
int32 run_count_0
int32 value_1
int32 run_count_1
...
```

校验规则：

- input length 必须能被 4 整除。
- decode 拒绝负数 count。
- decode 拒绝 run count 总和不等于 original count 的 payload。

适合长重复 run。

## `bitpack`

输入是 `uint32_t` 数组。编码输出保存：

```text
uint64 element_count
uint64 bit_width
packed payload bytes
```

encoder 会根据最大值选择最小 bit width，最小为 1。

校验规则：

- input length 必须能被 4 整除。
- decode 要求 `1 <= bit_width <= 32`。
- payload size 必须精确匹配 `element_count * bit_width`。

适合最大值较小的非负整数数组。

## `zstd`

可选 zstd frame compression。只有 CMake 在 `FCE_ENABLE_ZSTD=ON` 时找到 `zstd.h` 和 libzstd 才会编译启用。

运行时行为：

- `fce_codec_available(FCE_CODEC_ZSTD)` 返回 1 表示启用。
- encode 使用 `ZSTD_compress`，压缩等级 1。
- decode 要求 frame 带有已知 content size，并校验解压后 byte count 完全一致。
- 未启用 libzstd 时，encode/decode 返回 `FCE_ERR_UNSUPPORTED`。

适合较大的 opaque value，尤其是压缩率比 mmap zero-copy 更重要的场景。

## 对零拷贝的影响

pass-through value codec（`none`、`user_bytes`）可以返回直接指向 mmap 或 heap-loaded file 的 view。压缩 value codec 必须分配 reader-owned 或 iterator-owned 解码内存，因此 read path 不是完全 zero-copy。

## Key Codec 限制

压缩 key codec 只适合 exact lookup backend。Prefix 和 range 语义基于 canonical byte order，因此要求 pass-through key codec。Direct table 也要求 pass-through key，因为 slot addressing 依赖原始 `u64` bytes。
