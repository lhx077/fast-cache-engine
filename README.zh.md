# Fast Cache Engine

Fast Cache Engine 是一个小型 native C key/value cache 库，目标是把读路径做快、把缓存文件做成可校验、并且让 exact lookup 的正确性不依赖 hash 唯一性。

[English README](README.md)

## 它提供什么

- 稳定 C ABI，以及 Python `ctypes` 绑定。
- exact lookup 在 hash 定位后一定比较完整 canonical key bytes。
- 使用 `.fce` 内部文件的可持久化只读 cache 目录。
- sorted index、dense `u64` direct table、compact radix prefix scan、MPH-style read-only lookup、append log 等后端。
- raw bytes、user bytes、delta `int32`、RLE、bitpack，以及可选 zstd codec。
- mmap 优先的 reader，必要时 fallback 到 heap load。
- CRC64 校验、schema hash、版本拒绝、offset/length 边界校验。
- 内部 arena 内存管理，并暴露诊断用 leak 统计。
- CLI 支持 build、inspect、validate、dump、compact。

这个库不绑定业务语义：key 是 canonical bytes，value 是 opaque bytes。业务层负责对象编码、解码和解释。

## 快速开始

见 [快速开始](docs/QUICK_START.zh.md)，里面有 C、Python 和 CLI 示例。

最小 C 示例：

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
    /* value 在 reader close 前有效。 */
}
fce_reader_close(reader);
```

## 构建

```text
cmake -S . -B build
cmake --build build --config Release --parallel 1
```

构建选项：

- `FCE_ENABLE_ZSTD=ON`：找到 `zstd.h` 和 libzstd 时启用 zstd codec。

仓库最终不提交测试套件源码。GitHub Actions 会在 push 和 pull request 时临时生成 smoke tests 并验证。

## Cache 布局

一个 cache 是一个目录，内部文件统一使用 `.fce` 后缀：

```text
manifest.fce
index.fce
keys.fce
values.fce
log.fce
```

`manifest.fce` 保存 schema、backend、codec、version、size、checksum 和 backend metadata。reader 会在查询前拒绝不兼容版本和损坏 offset。

## 性能

本机 Release benchmark，默认 sorted-index 后端，byte key 类似 `key-123`，byte value 类似 `value-123`：

```text
10 万条记录：构建约 0.137 秒，打开约 0.057 秒，查询约 128 ns/次，约 780 万次/秒
100 万条记录：构建约 1.549 秒，打开约 0.590 秒，查询约 165 ns/次，约 605 万次/秒
```

最终仓库不提交 benchmark 源码和构建目标，上面的数字只作为参考数据保留在文档中。

## 文档

- [快速开始](docs/QUICK_START.zh.md)
- [API 参考](docs/API.zh.md)
- [文件格式](docs/FILE_FORMAT.zh.md)
- [后端设计](docs/BACKENDS.zh.md)
- [编码器](docs/CODECS.zh.md)

英文文档：

- [Quick Start](docs/QUICK_START.md)
- [API Reference](docs/API.md)
- [File Format](docs/FILE_FORMAT.md)
- [Backends](docs/BACKENDS.md)
- [Codecs](docs/CODECS.md)
