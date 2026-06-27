# 后端设计

Fast Cache Engine 在同一套 builder/reader API 后面提供五种 storage backend。所有 exact lookup backend 都保存 canonical key bytes，并在 hash 或 slot 命中后比较完整 key，避免 hash-only correctness risk。

## 选择建议

| Backend | 适合场景 | Freeze 后可变 | 主要查询代价 | Scan 支持 |
| --- | --- | --- | --- | --- |
| `sorted_index` | 通用 exact lookup，默认安全选择 | 否 | `O(log N + collisions)` | prefix、range、all |
| `direct_table` | 稠密 `u64` key | 否 | `O(1)` 数组 slot | all |
| `mph` | 高频只读 exact lookup | 否 | `O(1)` bucket/slot | all |
| `radix` | prefix/range byte-key 查询 | 否 | `O(key_len)` traversal | prefix、range、all |
| `log` | 在线 append 和导入 | append-only | `O(log N + duplicate scan)` | 建议先 compact |

## Sorted Index

Sorted index 是默认 backend。它支持任意 byte key，是最保守、最通用的选择。

构建流程：

```text
put records
encode keys/values
按 duplicate policy 去重
写 keys.fce 和 values.fce
为每个 canonical key 计算 hash128
按 hash_hi, hash_lo, key_len, key_offset 排序 index entries
写 index.fce 和 manifest.fce
```

查询流程：

```text
必要时编码 query key
计算 hash128
在 sorted index 中二分 hash
扫描 equal-hash range
比较完整 canonical key bytes
返回 value slice
```

优点：

- 支持任意 byte key。
- index 连续存储，适合 mmap。
- hash 碰撞下仍然正确。
- 支持 exact lookup、prefix scan、range scan 和 scan-all。

取舍：

- lookup 有二分成本。
- prefix/range scan 当前会遍历 index entry 并检查 key bytes；大量 prefix workload 更适合 radix。

## Direct Table

Direct table 把稠密 `u64` key 直接映射到 slot。

构建要求：

- `schema.key_kind` 必须是 `FCE_KEY_U64`。
- canonical key 必须正好 8 字节。
- 稀疏 keyspace 默认拒绝，除非设置 `FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE`。

查询流程：

```text
slot = slots[key]
if slot.present and slot.key_u64 == key:
    return value slice
else:
    miss
```

优点：

- 查询常数极小。
- 适合紧凑 dense ID 空间。
- slot array 可 mmap。

取舍：

- 稀疏 keyspace 会生成很大的文件。
- 只支持 `u64` 直接寻址。

## MPH

MPH backend 面向只读 exact lookup。当前实现为每条记录保存一个持久化 slot，并为每个 bucket 搜索 seed，把 bucket 内的 item 放入未使用的全局 slot。

构建流程：

```text
deduplicate records
写 keys.fce 和 values.fce
按 hash bucket 分组
优先处理最大 bucket
为每个 bucket 搜索 seed
把 bucket seeds 和 slots 写入 index.fce
```

查询流程：

```text
hash query key
bucket = hash_hi % bucket_count
seed = buckets[bucket].seed
slot = fnv1a64(key, seed) % slot_count
检查 slot.present
检查 stored hash
比较完整 canonical key bytes
返回 value slice
```

优点：

- `O(1)` lookup path。
- value slot 数等于 `record_count`。
- unknown key 即使映射到已有 slot，也会因为 exact key bytes 不相等而 miss。

取舍：

- 如果 seed search 在配置边界内无法放置 bucket，构建会返回 `FCE_ERR_UNSUPPORTED`。
- 最适合稳定只读 key set。

## Compact Radix

Radix 把 byte-key prefix tree 保存为 mmap-safe arrays：

- header
- node array
- sorted edge array
- terminal value metadata array

持久化文件里不保存 native pointer。node 通过 array index 引用 child 和 value。

查询流程：

```text
node = root
for byte in key:
    binary search sorted child edges
    node = child
check terminal value index
compare exact key bytes
return value slice
```

Prefix scan：

```text
walk prefix bytes to prefix node
depth-first traverse descendants
return terminal values
```

优点：

- prefix scan 自然高效。
- 支持 exact lookup 和 range scan。
- 持久化结构可 mmap，跨进程安全。

取舍：

- 对 dense integer key 不如 direct table。
- 对没有共享前缀的随机 key，不一定比 sorted index 更合适。

## Append Log

Log backend 支持在线 append。记录写在 `log.fce`：

```text
uint64 magic
uint64 key_len
uint64 value_len
key bytes
value bytes
uint64 record_crc64
```

reader open 会校验所有 record，并在内存中建立 transient hash-sorted index。重复 key 查询返回最新记录。

优点：

- 通过 `fce_log_append` 支持增量写入。
- freeze、append、inspect 和 reader-open 快照通过内部 cache lock 串行化，覆盖跨进程和同进程多线程写入。
- 已打开 reader 会持有共享锁直到 close，所以 writer 会等待，而不是覆盖仍被 mmap 的文件。
- 导入路径简单。
- 可以 compact 成 sorted、direct、radix 或 mph backend。

取舍：

- reader open 需要全量 log validation 和 transient index build。
- 文件会保留旧版本 duplicate value。
- 长期 read-heavy cache 应使用 `fce_compact` 冻结为更适合读取的 backend。
