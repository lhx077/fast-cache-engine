# Codecs

Codecs transform byte buffers before persistence or after reading. The library does not infer data type from bytes; the selected codec defines how the bytes are interpreted.

## Ownership

```c
FceStatus fce_codec_encode(FceCodecKind codec, const void *input, size_t input_len, void **out_data, size_t *out_len);
FceStatus fce_codec_decode(FceCodecKind codec, const void *input, size_t input_len, void **out_data, size_t *out_len);
```

Both functions allocate caller-owned output buffers. Release them with `fce_free`.

Builder APIs encode keys and values before writing. Reader APIs decode values when `schema.value_codec` is compressed. Query keys are encoded before lookup when `schema.key_codec` is compressed and the backend supports it.

## `none`

Byte-for-byte copy.

Use when:

- values are already in final binary form
- mmap zero-copy reads are important
- key bytes must preserve lexical prefix/range semantics

## `user_bytes`

Byte-for-byte copy for application-defined encodings.

Use when the application owns its own serialization format. Fast Cache Engine treats the bytes as opaque and only stores, validates, and returns them.

## `delta_i32`

Input is an array of little-endian/native `int32_t` values. Encoded output stores:

```text
first value
delta(value[1] - value[0])
delta(value[2] - value[1])
...
```

Validation:

- input length must be divisible by 4
- encode rejects signed 32-bit delta overflow
- decode rejects reconstructed signed 32-bit overflow

Best for monotonic or slowly changing integer arrays.

## `rle`

Input is an array of `int32_t` values. Encoded output stores:

```text
int32 original_count
int32 value_0
int32 run_count_0
int32 value_1
int32 run_count_1
...
```

Validation:

- input length must be divisible by 4
- decode rejects negative counts
- decode rejects payloads whose run counts do not sum to the stored original count

Best for long repeated runs.

## `bitpack`

Input is an array of `uint32_t` values. Encoded output stores:

```text
uint64 element_count
uint64 bit_width
packed payload bytes
```

The encoder chooses the minimum bit width needed by the maximum value, with a minimum width of 1.

Validation:

- input length must be divisible by 4
- decode requires `1 <= bit_width <= 32`
- payload size must exactly match `element_count * bit_width`

Best for non-negative integer arrays with small maximum values.

## `zstd`

Optional zstd frame compression. This codec is compiled only when CMake finds `zstd.h` and a libzstd library while `FCE_ENABLE_ZSTD=ON`.

Runtime behavior:

- `fce_codec_available(FCE_CODEC_ZSTD)` returns 1 when enabled.
- encode uses `ZSTD_compress` with compression level 1.
- decode requires a frame with known content size and validates the exact decompressed byte count.
- when libzstd is not available, encode/decode return `FCE_ERR_UNSUPPORTED`.

Use zstd for larger opaque values where compression ratio matters more than pure mmap zero-copy reads.

## Zero-Copy Impact

Pass-through value codecs (`none`, `user_bytes`) can return direct mmap or heap-loaded file views. Compressed value codecs must allocate decoded reader-owned or iterator-owned memory, so they are not zero-copy on read.

## Key Codec Restrictions

Compressed key codecs are only appropriate for exact lookup backends. Prefix and range semantics operate on canonical byte order, so they require pass-through key codecs. Direct table also requires pass-through keys because slot addressing depends on the exact `u64` bytes.
