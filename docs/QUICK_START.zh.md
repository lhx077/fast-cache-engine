# 快速开始

这个文档展示如何构建一个小 cache、读回数据，以及 CLI 输入格式。

## 构建

```text
cmake -S . -B build
cmake --build build --config Release --parallel 1
```

CI 验证会在 push 和 pull request 时运行：

```text
git push
```

最终仓库不提交测试源码。GitHub Actions 会在 push 和 pull request 时临时生成 smoke tests 并验证。

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

返回的 value pointer 归 reader 所有，在 `fce_reader_close` 前有效。

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

Python 返回 `memoryview`。如果 value 需要脱离 reader 生命周期，请转换成 `bytes`。

## CLI

CLI 的 `build` 命令读取 length-prefixed record stream：

```text
uint64 key_len
uint64 value_len
key bytes
value bytes
...
```

示例命令：

```text
fce build --schema schema.json --input records.fce --output cache_dir
fce inspect cache_dir
fce validate cache_dir
fce dump cache_dir --limit 10
```

compact 可以把已有 cache 重写成另一个 backend：

```text
fce compact log_cache --backend sorted_index --output frozen_cache
```

## 后端选择

- 默认 exact lookup 使用 `sorted_index`。
- 稠密 `u64` id 使用 `direct_table`。
- prefix scan 使用 `radix`。
- 稳定只读 key set 使用 `mph`。
- 在线 append 使用 `log`，读多时再 compact。
