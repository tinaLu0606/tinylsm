# TinyLSM Goal 3 · Compaction 设计与验收记录

状态：`进行中`

## 策略决定

TinyLSM 选择**简化 size-tiered**，而不是引入 leveled layout。

```text
oldest                                             newest
[T0] [T1] [T2] [T3] [T4] [T5]
 \_________________/                 unchanged
       merge to R

[R] [T4] [T5]
```

输入固定为最老且连续的 `N` 张表，默认 `N = 4`。相比简化 leveled，它不需要新增
level metadata、overlap selection 或跨 level 的 VersionEdit；当前 Manifest 已保证
SSTable sequence range 从旧到新严格递增，因此 replacement 可以放回同一个前缀位置。

这不是通用 RocksDB/LevelDB compaction。它的目标是先有一个可证明正确、内存和队列
均有界的自动 table-set 管理基线，再由测量决定是否值得承担 leveled 的格式与调度复杂度。

## Tombstone 规则

partial compaction 只选最老表前缀。因此其输入左侧没有未参与的、更旧的表；一个来自
输入的 tombstone 不可能还需要遮蔽外部旧值，可以删除。输入右侧的表和 MemTable 都有
更高 sequence，仍会在 Get/Scan 中覆盖 replacement。

```text
safe to drop:   [old value, tombstone] [newer tables only]
unsafe shape:   [older table] [selected tombstone run]
```

第二种形态不是本策略的输入。代码的安全条件是“selected run starts at table index 0”，
而不是“tombstone 看起来是最新的”。这避免删除掩码后令旧值复活。

## Publication and visibility

后台 job 先在 Manifest 中 durable 地前进 `next_file_number`，保留 output number，随后
在锁外迭代 `shared_ptr<SSTableReader>` snapshot。此 reservation 防止同时发生的 WAL
rotation/flush 复用同一文件号。

```text
version lock: snapshot oldest N + reserve file number (Manifest durable)
                                      |
                                      v
outside lock: merge / build / validate / rename / SyncDir output SST
                                      |
                                      v
version lock: verify same prefix -> replace prefix in Manifest -> publish
                                      |
                                      v
drop old readers/cache entries, best-effort unlink old SSTs
```

Manifest update, table-reader replacement and debt gauge refresh all use the same state lock.
读者保有自己的 reader snapshot；后台合并不持有 reader lock，因此新的 Get 可以与后台
input I/O 重叠。Manifest commit 前旧表仍 authoritative；rename-visible 但 directory-sync
失败时 handle 进入 terminal error，必须 close/reopen。

explicit `Compact()` 仍保持历史 fault matrix 的单 Manifest commit：它持独占状态锁、等待
后台任务结束、同步重写完整 table set。它是恢复/调试操作，不是新的自动策略。

## Metrics contract

| 目标 | 原始计数 | 计算方式 |
| --- | --- | --- |
| Read amplification | `point_lookups`, `table_probes` | `table_probes / point_lookups` |
| Write amplification | `logical_write_bytes`, `flush_output_bytes`, `compaction_output_bytes` | `(flush + compaction output) / logical writes` |
| Space amplification | `live_sstable_bytes`, workload logical-live bytes | `live SST bytes / logical-live bytes` |
| Table pressure | `table_count`, `compaction_debt_tables`, `compaction_debt_bytes` | 直接报告 |
| Foreground tail | sampled Put/Get/Scan latency | p50/p95/p99；不混入 Close drain |

所有指标均是 handle 的累计快照或当前 gauge；benchmark/report 必须同时保留 raw JSON，
不能只保存综合分数。

## 当前验证与剩余验收

已验证：partial tombstone 不复活、background Manifest commit failure reopen、reader 与
background input I/O overlap，以及当前 Debug full suite。尚未完成 fixed-Linux mixed
workload、sanitizer、长期 crash/reopen、最终放大率和前台尾延迟报告；这些完成前，本文件
不构成 Goal 3 性能结论。
