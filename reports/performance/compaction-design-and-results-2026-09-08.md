# TinyLSM Goal 3 · Compaction 设计与验收记录

状态：`已完成`

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

选择依据是同一固定 Linux mixed workload 下的实测：关闭自动调度的基线在计时后才做
full `Compact()`，所以其最终 table count 会变成 1；但计时中的 point-read amplification
仍约为 16.4。size-tiered 在同一 workload 后报告 3 个表、零 debt，并把读放大降到约 2.14，
同时保持 Put p99 更低。现阶段没有实现一个可比的 leveled layout，因此这不是
“size-tiered 胜过已测 leveled”的结论；而是 size-tiered 已满足当前 table-pressure 和
前台读尾延迟目标，缺乏证据支持现在引入更多 persistent layout/scheduler 复杂度。

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

## Measurement method

每个 case 使用 20,000 个操作、4,096 个 key、256-byte value、64 KiB MemTable、关闭
block cache、`sync_on_write=false`，并以空临时 DB 执行五次。操作序列混合 sequential、
64-key hot set 和 uniform key；每 100 次操作包含一次 Delete、一次 Scan、98 次 Get，
其余为 Put。`Manual` 将 automatic trigger 设为零，并在前台计时结束后调用 explicit
full `Compact()`；`SizeTiered` 使用默认 trigger=4。因此 throughput 和 p50/p95/p99
只描述前台混合循环，不把 Manual 的后置 full compaction 或 Close drain 算入延迟。

两轮均在固定 Lima Linux VM（Ubuntu 24.04 ARM64、Linux 6.8.0-134、4 vCPU、7.7 GiB
RAM、ext4、GCC 13.3、Clang 18.1）完成。每轮运行 Debug、ASan/UBSan、Clang TSan、
Release，以及 JSON verifier。外层第 1 轮 benchmark wrapper 因首次构建花 3:31，
第 2 轮为 0:15；这不是 workload 结果，以下只使用 JSON 内五次运行的 benchmark
median 和 CV。

## Fixed-Linux results

| Case | ops/s median, run 1 / run 2 | read amp | write amp | space amp | tables / debt | compactions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Manual full after timer | 10,358 / 10,475 | 16.420 / 16.405 | 1.286 / 1.286 | 1.082 / 1.082 | 1 / 0 | 1 / 1 |
| Background size-tiered | 19,266 / 19,640 | 2.140 / 2.135 | 7.120 / 7.120 | 1.181 / 1.181 | 3 / 0 | 33 / 33 background |

`read amp = table_probes / point_lookups`，`write amp = (flush_output_bytes +
compaction_output_bytes) / logical_write_bytes`，`space amp = live_sstable_bytes /
logical_live_bytes`。两种策略的 logical write bytes 都是 5,280,000，logical live bytes
都是 1,101,872；这些原始计数、input/output byte 和 table/debt gauges 均在 JSON 中保留。
size-tiered 的 33 次合并读取 36,292,643 bytes、输出 31,997,607 bytes，这是它以约 5.54x
更高 write amplification 换取约 7.68x 更低 point-read amplification 的明确成本。

| Case | Put p50 / p95 / p99 us, run 1 / run 2 | Get p50 / p95 / p99 us, run 1 / run 2 | Scan p50 / p95 / p99 us, run 1 / run 2 |
| --- | --- | --- | --- |
| Manual | 2.508 / 5.398 / 69.677; 2.503 / 5.381 / 67.084 | 870.962 / 4,763.852 / 5,704.958; 870.001 / 4,742.043 / 5,672.292 | 6,692.194 / 18,666.870 / 20,933.690; 6,583.498 / 17,957.622 / 20,640.158 |
| Size-tiered | 2.511 / 5.230 / 33.308; 2.502 / 5.215 / 32.734 | 205.092 / 507.769 / 611.114; 203.745 / 496.430 / 614.173 | 617.542 / 1,150.068 / 1,362.391; 616.328 / 1,154.356 / 1,526.056 |

Manual throughput CV is 2.33% and 0.24%; size-tiered is 9.40% and 1.17%.
All retained cases are below 10%. Size-tiered roughly doubles foreground
throughput, halves Put p99, and sharply reduces Get/Scan tails on this workload;
the result does not establish a general write-throughput or p99 guarantee.

Raw artifacts:

- `results/goal3-compaction-linux-2026-09-08-run1.json`
- `results/goal3-compaction-linux-2026-09-08-run2.json`
- `results/goal3-compaction-linux-2026-09-08-run1-environment.txt`
- `results/goal3-compaction-linux-2026-09-08-run2-environment.txt`
- `results/goal3-compaction-linux-2026-09-08-run1-benchmark.log`
- `results/goal3-compaction-linux-2026-09-08-run2-benchmark.log`

Run `./scripts/run_linux_compaction_goal3.sh <output-dir>` inside the same Linux
VM to retain the complete Debug/ASan/TSan/Release logs, raw JSON and verification
output for another run.

## Correctness and acceptance

The final local evidence is Debug, ASan/UBSan and TSan all at 107/107, plus a
Release build. Each retained fixed-Linux run repeated the same three 107/107
suites with no sanitizer report, completed the Release build, and passed
`scripts/verify_compaction_benchmark_json.cmake`.

After the two benchmark runs, a clean Git-bundle checkout of revision `746741e`
also built Release on the fixed Linux ext4 VM and passed a CLI deployment smoke:
`put deployment linux-ok`, separate-process reopen/get, text Scan, and Base64
JSON Scan. The environment, build, command and assertion logs are stored as
`results/goal3-release-smoke-linux-2026-09-08-*`. The independent Lab UI
submodule was not initialized because this smoke validates only the Release CLI.

Correctness coverage includes oldest-prefix tombstone retention, failed background
Manifest commit followed by reopen, rename-visible/SyncDir terminal behavior,
reader overlap with the compaction input snapshot, and a 5,000-operation mixed
Put/Delete/Get/Scan reference-model workload checked again after reopen. These
are deterministic crash/reopen/fault boundaries; they do not simulate a physical
machine power loss or prove a multi-level compaction policy.
