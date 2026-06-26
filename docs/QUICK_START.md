# Quick Start

This guide builds a small cache, reads it back, and shows the CLI input format.

## Build

```text
cmake -S . -B build
cmake --build build --config Release --parallel 1
```

CI validation runs on push and pull request:

```text
git push
```

The final repository does not ship checked-in test sources. GitHub Actions generates smoke tests during push and pull request validation.

## C

```c
#include "fce_cache.h"
#include <string.h>

int main(void) {
    FceSchema schema = fce_schema_default();
    FceBuilder *builder = NULL;
    FceReader *reader = NULL;

    if (fce_builder_open("cache", &schema, &builder) != FCE_OK) return 1;
    if (fce_builder_put(builder, "user:1", 6, "Ada", 3) != FCE_OK) return 1;
    if (fce_builder_freeze(builder) != FCE_OK) return 1;
    fce_builder_close(builder);

    const void *value = NULL;
    size_t value_len = 0;
    if (fce_reader_open("cache", &reader) != FCE_OK) return 1;
    if (fce_reader_get(reader, "user:1", 6, &value, &value_len) != FCE_OK) return 1;
    if (value_len != 3 || memcmp(value, "Ada", 3) != 0) return 1;
    fce_reader_close(reader);
    return 0;
}
```

Returned value pointers are owned by the reader and stay valid until `fce_reader_close`.

## Python

```python
from fast_cache_engine import CacheBuilder, CacheReader, CacheSchema

schema = CacheSchema(backend="sorted_index")

with CacheBuilder("cache", schema) as builder:
    builder.put(b"user:1", b"Ada")
    builder.freeze()

with CacheReader("cache") as reader:
    value = reader.get(b"user:1")
    assert bytes(value) == b"Ada"
```

Python values are returned as `memoryview` objects. Convert them to `bytes` if the value must outlive the reader.

## CLI

The CLI `build` command reads a length-prefixed record stream:

```text
uint64 key_len
uint64 value_len
key bytes
value bytes
...
```

Example commands:

```text
fce build --schema schema.json --input records.fce --output cache_dir
fce inspect cache_dir
fce validate cache_dir
fce dump cache_dir --limit 10
```

Compaction rewrites an existing cache into another backend:

```text
fce compact log_cache --backend sorted_index --output frozen_cache
```

## Backend Choice

- Use `sorted_index` as the default exact lookup backend.
- Use `direct_table` for dense `u64` ids.
- Use `radix` for prefix scans.
- Use `mph` for stable read-only key sets.
- Use `log` for online append, then compact it for read-heavy use.
