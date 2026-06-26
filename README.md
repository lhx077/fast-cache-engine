# Fast Cache Engine

Fast Cache Engine is a small native C library for building read-optimized key/value caches. It is designed for workloads where lookup cost matters, cache files should be verifiable, and correctness must not depend on hash uniqueness.

[中文 README](README.zh.md)

## What It Provides

- Stable C ABI with a Python `ctypes` binding.
- Exact lookup that always verifies full canonical key bytes after hash-based positioning.
- Read-optimized persistent cache directories using `.fce` files.
- Backends for sorted indexes, dense `u64` direct tables, compact radix prefix scans, MPH-style read-only lookup, and append logs.
- Optional codecs for raw bytes, user bytes, delta `int32`, RLE, bitpack, and zstd when libzstd is available.
- mmap-first readers with heap fallback, CRC64 validation, schema hashing, and version rejection.
- Internal arena-based memory management with leak statistics exposed for diagnostics.
- CLI tools for build, inspect, validate, dump, and compact.

Fast Cache Engine is domain-neutral. It stores canonical key bytes and opaque value bytes; application encoding stays outside the library.

## Quick Start

See [Quick Start](docs/QUICK_START.md) for C, Python, and CLI examples.

Minimal C example:

```c
#include "fce_cache.h"

FceSchema schema = fce_schema_default();
FceBuilder *builder = NULL;
FceReader *reader = NULL;

fce_builder_open("cache", &schema, &builder);
fce_builder_put(builder, "key", 3, "value", 5);
fce_builder_freeze(builder);
fce_builder_close(builder);

const void *value = NULL;
size_t value_len = 0;
fce_reader_open("cache", &reader);
if (fce_reader_get(reader, "key", 3, &value, &value_len) == FCE_OK) {
    /* value is valid until the reader is closed. */
}
fce_reader_close(reader);
```

## Build

```text
cmake -S . -B build
cmake --build build --config Release --parallel 1
```

Build option:

- `FCE_ENABLE_ZSTD=ON`: enable zstd codec when `zstd.h` and libzstd are found.

The repository does not ship a checked-in test suite. GitHub Actions generates smoke tests during push and pull request validation.

## Cache Layout

A cache is a directory. Internal files use the `.fce` suffix:

```text
manifest.fce
index.fce
keys.fce
values.fce
log.fce
```

`manifest.fce` stores schema, backend, codec, version, size, checksum, and backend metadata. Readers reject incompatible versions and corrupted offsets before serving lookups.

## Performance

Local Release benchmark on this machine, default sorted-index backend, byte keys like `key-123`, byte values like `value-123`:

```text
records=100000  build_seconds=0.137  open_seconds=0.057  lookup=128 ns/lookup  ~= 7.8M lookups/s
records=1000000 build_seconds=1.549  open_seconds=0.590  lookup=165 ns/lookup  ~= 6.1M lookups/s
```

Benchmark source and build targets are not shipped in the final repository. The numbers above are retained as reference data only.

## Documentation

- [Quick Start](docs/QUICK_START.md)
- [API Reference](docs/API.md)
- [File Format](docs/FILE_FORMAT.md)
- [Backends](docs/BACKENDS.md)
- [Codecs](docs/CODECS.md)

Chinese docs:

- [快速开始](docs/QUICK_START.zh.md)
- [API 参考](docs/API.zh.md)
- [文件格式](docs/FILE_FORMAT.zh.md)
- [后端设计](docs/BACKENDS.zh.md)
- [编码器](docs/CODECS.zh.md)
