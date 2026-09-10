# TinyLSM 优化后综合性能报告 · 2026-09-09

状态：固定 Linux VM 的 Goals 4–6 两轮 Release benchmark 已完成；38/38 个 case
均有完整 median，跨两轮 real-time CV 全部不超过 10%。随后完成写入回归修复及 12/12 个
受影响 case 的两轮定向复测，最大跨轮 CV 为 `9.309%`。本报告同时汇总历史 baseline
与 Goals 1–3 的可比证据。2026-09-10 补充了优化后与 LevelDB 的同 session 对比，见
Goal 3 之后的独立小节。

## 结论摘要

TinyLSM 的优化结果不是一个可以诚实压缩成“整体提升 X%”的数字：不同 Goal 使用不同
workload。本次完整测试发现了一个严重的单操作写入回归，随后通过修复前后定向 A/B
完成了原因确认和闭环。可以成立的结论是：

- Goal 1 的 decoded Block Cache 是目前最强的已验证收益。single-SSTable hit 从约
  `10.1k` 提升到 `1.36M ops/s`，约 `134–135x`；四表 oldest hit 提升约
  `257–259x`。
- Goal 3 的 simplified size-tiered compaction 在定义的 mixed workload 中把吞吐从
  `10.4k` 提高到 `19.3–19.6k ops/s`，同时把 read amplification 从约 `16.4`
  降到约 `2.14`；代价是 write amplification 从 `1.286` 升到 `7.120`。
- Goal 4 的 pull Iterator 相比 materialized Scan，完整遍历吞吐高 `32.6–42.9%`，
  首条结果快 `3.26–3.83x`。释放最老 Snapshot 后，60s-equivalent workload 的
  retained versions/bytes 和 live SST bytes 均回收约 `85.6–85.7%`。
- Goal 5 的 SSTable v2 对共享前缀 key 最有效：restart interval 16/64 将 block
  大小减少 `26.2–27.7%`，代表性 lookup 提升 `40.6–42.4%`。随机 key 的空间收益
  只有约 `0.4–2.7%`，但 lookup 仍提高 `5.6–8.4%`。
- Goal 6 的 Group Commit 在 1–2 writers 下没有形成物理合并，也没有稳定吞吐收益；从
  4 writers 开始有效。修复后的 4/8/16 writers 吞吐分别提高 `24.6–33.4%`、
  `158.3–158.9%`、`336.9–348.9%`，WAL sync/write 约为 `0.746`、`0.371–0.372`、
  `0.187`。
- 单操作写入曾因 `MemTable::ApplyBatch()` 每次复制整张 MemTable 而形成近似
  `O(N^2)` 的累计成本。改为只暂存 batch 触及的 key 后，writer overlap 从坏版本的
  `676–692 ops/s` 恢复到 `5.05–5.98M ops/s`，writer p50 从 `31.6–32.6 ms`
  恢复到 `1.792–1.875 us`。这确认了回归原因并消除了数量级退化。

因此，当前可以对读取、compaction、Snapshot Iterator、SSTable v2、高并发 Group
Commit，以及本次单操作写入回归修复做有边界的量化陈述；仍不能做“整个引擎全面变快”
这类跨 workload 的笼统陈述。

## 正式测量环境与证据

正式 benchmark 平台是固定 Linux VM；macOS 仅用于开发和 VM host，不提供正式数字。

| 项目 | 值 |
| --- | --- |
| Guest | Ubuntu 24.04 ARM64, Linux `6.8.0-134` |
| CPU / memory | 4 vCPU / 7.7 GiB RAM，无 swap |
| Persistent filesystem | guest-local `/dev/vda1`, ext4 |
| Compiler | GCC 13.3.0；Clang 18.1.3 已记录但本次 Release 使用 GCC |
| Build | CMake 3.28.3, Ninja 1.11.1, Release |
| Google Benchmark | v1.9.5 |
| Source | exported snapshot, SHA-256 `ef480687a6727112946caf6d956faf781e44f2d7b433b1aabfa3ed7e7a4619c0` |
| Run 1 | 33:18.90，exit 0，wrapper max RSS 1,130,496 KiB |
| Run 2 | 30:19.01，exit 0，wrapper max RSS 1,112,116 KiB |

每个 case 固定 `Iterations(1) × Repetitions(5)`，报告每轮 median。本次 verifier
还对两个独立 run 的 real-time median 计算 CV；最大值为
`3.645%`（SSTable v2 prefix encode, restart 64），38 个 case 全部低于 `10%`。

这不表示所有五次样本中的所有 counter 都同样稳定。个别 case 的单轮 real-time CV
超过 10%，Group Commit 的部分 p99 也有较大波动；所以吞吐 median 可以作为正式证据，
尾延迟只在两轮方向一致时作趋势结论，不写成稳定 SLA。

完整证据：

- [Run 1 JSON](results/goal4-6-linux-2026-09-09/run1/next-goals.json)
- [Run 2 JSON](results/goal4-6-linux-2026-09-09/run2/next-goals.json)
- [环境](results/goal4-6-linux-2026-09-09/environment.txt)
- [两轮 CV 校验](results/goal4-6-linux-2026-09-09/verification.log)
- [精确源码归档](results/goal4-6-linux-2026-09-09/tinylsm-source-20260909-v2.tar.gz)

写入修复后的定向复测使用相同 VM、构建类型、次数与判定阈值，只运行两个
writer-overlap case 和 10 个 Group Commit case。12 个 case 的跨轮 CV 均不超过
`9.309%`，源码归档 SHA-256 为
`d9f631362c1b6fde4d9ffcb15b0a251c891f3cc9e81c297e3b50f7859640cfe6`：

- [修复后 Run 1 JSON](results/write-regression-fix-linux-2026-09-09/run1/next-goals.json)
- [修复后 Run 2 JSON](results/write-regression-fix-linux-2026-09-09/run2/next-goals.json)
- [修复后环境](results/write-regression-fix-linux-2026-09-09/environment.txt)
- [修复后 Release 构建日志](results/write-regression-fix-linux-2026-09-09/release-build.log)
- [修复后 Run 1 日志](results/write-regression-fix-linux-2026-09-09/run1/benchmark.log)
- [修复后 Run 2 日志](results/write-regression-fix-linux-2026-09-09/run2/benchmark.log)
- [修复后两轮 CV 校验](results/write-regression-fix-linux-2026-09-09/verification.log)
- [修复后精确源码归档](results/write-regression-fix-linux-2026-09-09/tinylsm-source-20260909-writefix.tar.gz)

## 从初版 baseline 到 Goals 1–3

初版报告是 [baseline-2026-09-06.md](baseline-2026-09-06.md)。它记录优化前的固定
Linux 行为，并明确指出 TinyLSM 当时在 async write、read hit 和 Scan 上明显落后于
LevelDB。后续 Goal 的 workload 更专门化，因此只有定义一致的 case 才做倍率比较。

### Goal 1：读取路径

| Workload | Before run 1 → after run 1 | Before run 2 → after run 2 | 提升 |
| --- | ---: | ---: | ---: |
| Single-SSTable hit | 10,134 → 1,358,407 | 10,114 → 1,368,248 | 134.0x / 135.3x |
| Four-table oldest hit | 2,563 → 663,208 | 2,575 → 662,924 | 258.8x / 257.4x |
| Four-table in-range miss | 2,571 → 670,210 | 2,578 → 671,226 | 260.7x / 260.4x |
| Four-table repeated Scan | 1,070,776 → 8,822,380 | 1,082,360 → 8,830,810 | 8.24x / 8.16x |

profile 将重复 CRC/decode 定位为主要成本，随后加入只缓存已完整验证 block 的有界 LRU
cache。这一结果支持 cache 决策，但不支持跳过 checksum。完整方法与 11-case raw JSON
见 [read-path-2026-09-07.md](read-path-2026-09-07.md)。

### Goal 2：有界异步写入

在 256-byte value、256 KiB MemTable 的专项 workload 中，async 两轮为
`123,050 / 127,375 ops/s`，sync 为 `4,775 / 4,773 ops/s`；两者分别覆盖
100,000 和 20,000 次写入，不能把倍率解释成同 workload 的优化百分比。结果证明一个
immutable generation、后台 flush 与有界 backpressure 能稳定运行，但当时尚未实现
Group Commit。详情见 [write-path-2026-09-07.md](write-path-2026-09-07.md)。

### Goal 3：Compaction

| 策略 | 吞吐 run 1 / run 2 | Read amp | Write amp | Space amp | Put p99 run 1 / run 2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Manual | 10,358 / 10,475 | 16.420 / 16.405 | 1.286 | 1.082 | 69.677 / 67.084 us |
| Size-tiered | 19,266 / 19,640 | 2.140 / 2.135 | 7.120 | 1.181 | 33.308 / 32.734 us |

size-tiered 在这个 mixed workload 中约提高 `86–88%` 吞吐、把 point-read
amplification 降低约 `7.7x`，但付出约 `5.54x` 的 write-amplification 代价。
它证明当前简化策略有价值，不等同于证明 size-tiered 普遍优于 leveled。详情见
[compaction-design-and-results-2026-09-08.md](compaction-design-and-results-2026-09-08.md)。

### 优化后与 LevelDB 的同 session 对比

上面每个 Goal 的报告都只和 TinyLSM 自己的 before/after 比较；`baseline-2026-09-06.md`
是唯一同时跑过 TinyLSM 和 LevelDB 的报告，但那是优化前的快照。2026-09-10 用同一套
baseline harness、在同一个固定 Linux VM 里对当前 revision（含 Goal 1-6 与单操作写入
回归修复）重新同时跑了两轮 TinyLSM 和 LevelDB。结论不是单一的"快了多少"：随机读命中
和顺序异步写有真实提升（约 2x / 64-66%），但全量 compaction 和部分并发写场景在这个
通用 harness 里持平或变慢——已定位到全量 compaction 变慢是因为 harness 的计时边界
被 Goal 3 新增的后台 compaction 打破（`Compact()` 现在要等后台任务收尾），不是
compaction 本身变慢。完整数据、方法说明和根因分析见
[post-optimization-vs-leveldb-2026-09-10.md](post-optimization-vs-leveldb-2026-09-10.md)。

## Goal 4：Snapshot、Iterator 与 MVCC GC

### Iterator 对 materialized Scan

以下 ops/s 表示每秒遍历的 entry 数，每次 repetition 执行 10 次完整遍历。数据库准备
不在手工计时区间内。

| Entries | Scan ops/s r1 / r2 | Iterator ops/s r1 / r2 | Iterator 吞吐提升 | 首条结果 Scan / Iterator | 首条结果加速 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10k | 7.463M / 7.441M | 10.422M / 10.630M | +39.7% / +42.9% | 1.177 / 1.177 ms vs 0.310 / 0.307 ms | 3.80x / 3.83x |
| 100k | 7.265M / 7.426M | 9.950M / 10.295M | +36.9% / +38.6% | 12.029 / 11.800 ms vs 3.326 / 3.278 ms | 3.62x / 3.60x |
| 1m | 7.124M / 7.197M | 9.550M / 9.545M | +34.1% / +32.6% | 123.432 / 121.852 ms vs 37.076 / 37.370 ms | 3.33x / 3.26x |

Iterator 的优势来自按需产出结果并避免构造完整 public result vector。`peak_rss_kb`
来自同一随机交错 benchmark 进程；allocator 保留和先前大 case 会污染后续 VmRSS，故本次
不把约 1 GiB 的逐 case 数字解释为 Iterator 的独立内存节省。

### Snapshot retention 与释放

| 状态 | Retained versions | Retained bytes | Live SST bytes |
| --- | ---: | ---: | ---: |
| Held, 0 generation | 2,000 | 232,000 | 251,927 |
| Held, 1 generation | 4,000 | 464,000 | 501,444 |
| Held, 6 generations | 14,005 | 1,624,050 | 1,749,903 |
| Released, 6 generations | 2,005 | 232,050 | 252,048 |

在相同 6-generation workload 下释放 Snapshot 后，retained versions 减少
`85.68%`、retained bytes 减少 `85.71%`、live SST bytes 减少 `85.60%`。
这证明 watermark GC 生效；仍保留约一个当前版本/每 key，是正确的 live state，不是泄漏。

### Writer overlap 回归与修复

| Traversal | 2026-09-08 ops/s r1 / r2 | 坏版本 ops/s r1 / r2 | 修复后 ops/s r1 / r2 | 修复后 p50 r1 / r2 |
| --- | ---: | ---: | ---: | ---: |
| Materialized Scan | 7.071M / 5.113M | 675 / 692 | 5.765M / 5.053M | 1.792 / 1.875 us |
| Iterator | 8.909M / 7.409M | 680 / 676 | 5.983M / 5.970M | 1.792 / 1.792 us |

坏版本中两个 traversal 的结果几乎相同，说明主要成本不在 Scan/Iterator 持锁差异，而在
并发 writer。本次写队列把单条 `Put` 包装成单操作 batch，`ApplyWriteGroup()` 最终调用
`MemTable::ApplyBatch()`；旧实现为 allocation-failure 原子性复制整张 `std::map`。
随着 MemTable 增长，每次 Put 成本线性增加，累计成为近似 `O(N^2)`。

修复分成两个阶段：先校验完整 batch 的 sequence 和内存计数，并为 batch 触及的 key
构造完整 replacement vectors；再通过 `vector::swap` 和 C++17 map node transfer
提交。这样保留 allocation-failure 原子性，提交前的复杂度只与受影响 key 及其版本有关，
不再复制未触及的 MemTable 内容。修复后吞吐提高约四个数量级、p50 回到微秒级，定向 A/B
确认上述因果关系。

修复后 Materialized Scan 与 2026-09-08 的两轮范围基本重叠；Iterator 仍低约
`19–33%`，p99 也略高。因此这里能声称“灾难性回归已修复”，不能声称 Goal 6 引入的
writer queue、batch 包装等额外开销已经完全消失。

## Goal 5：SSTable v2 prefix compression

### Shared-prefix keys

| Restart | 大小相对 v1 | Encode ops/s 相对 v1 r1 / r2 | Lookup ops/s 相对 v1 r1 / r2 |
| ---: | ---: | ---: | ---: |
| 4 | -20.6% | +22.0% / +22.6% | +31.8% / +31.8% |
| 16 | -26.3% | +30.8% / +31.6% | +40.6% / +41.9% |
| 64 | -27.7% | +28.5% / +34.0% | +42.0% / +42.4% |

v1 为约 `170.0 bytes/entry`；v2 restart 16/64 分别为约 `125.35` 和
`122.92 bytes/entry`。共享前缀 workload 同时获得空间和 CPU 收益，说明减少编码/解码
字节足以抵消 prefix reconstruction 成本。

### Pseudo-random keys

| Restart | Encode block 大小相对 v1 | Lookup block 大小相对 v1 | Encode 提升 r1 / r2 | Lookup 提升 r1 / r2 |
| ---: | ---: | ---: | ---: | ---: |
| 4 | -1.4% | -0.4% | +1.2% / +0.3% | +7.8% / +5.6% |
| 16 | -2.4% | -1.2% | +1.8% / +1.0% | +8.4% / +6.8% |
| 64 | -2.7% | -1.5% | +2.2% / +1.8% | +7.5% / +6.4% |

随机 key 缺少可压缩前缀，所以空间收益很小。restart 16 在空间、随机 lookup 和 restart
粒度间提供更均衡的默认值；restart 64 对共享前缀空间最好，但不能据此声称它对所有 key
分布最优。这里测量的是 data-block codec，不是 Open、Block Cache 或全 DB Scan。

## Goal 6：有界 Group Commit

每个 case 固定总计 8,192 次、256-byte、`sync_on_write=true` 的 Put。Off 与 On 使用
同一有界 writer queue；唯一主要变量是每个物理 group 最多 1 或 8 个 caller request。

| Writers | Off ops/s r1 / r2 | On ops/s r1 / r2 | 吞吐变化 | On sync/write r1 / r2 | p99 Off → On r1 / r2 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5,529 / 5,345 | 5,068 / 5,174 | -8.4% / -3.2% | 1.000 / 1.000 | 255→290 / 260→272 us |
| 2 | 5,304 / 5,061 | 5,376 / 5,187 | +1.4% / +2.5% | 1.000 / 1.000 | 511→447 / 517→491 us |
| 4 | 5,292 / 5,025 | 6,596 / 6,705 | +24.6% / +33.4% | 0.746 / 0.747 | 670→2,463 / 697→501 us |
| 8 | 5,216 / 5,050 | 13,506 / 13,045 | +158.9% / +158.3% | 0.371 / 0.372 | 2,707→531 / 1,580→546 us |
| 16 | 5,339 / 5,258 | 23,970 / 22,972 | +348.9% / +336.9% | 0.187 / 0.187 | 5,043→610 / 3,411→594 us |

1–2 writers 的 `max_writer_queue_depth` median 为 1，On 仍执行 8,192 次 physical group
和 WAL sync，因此没有合并机会。4/8/16 writers 的 On median physical groups 分别为
`6,111–6,117`、`3,036–3,051`、`1,530–1,531`，吞吐随 sync 摊销明显提高。

8 和 16 writers 的 p99 改善方向在两轮一致。4 writers 的第一轮 On p99 出现
`2.463 ms` 尾部波动，与第二轮方向相反；因此 4 writers 只能声称吞吐收益，不能声称
稳定改善 tail latency。所有 p99 仍是观察值，不是 SLA。

这个修复后 A/B 证明“已有并发积压时 group commit 能减少 sync 并提高吞吐”。修复前
Group Commit 数据仍保留在原始 38-case JSON 中，用于说明问题发现过程，不再作为最终
绝对吞吐结论。

## 测试过程中的方法修正

第一次正式启动使用逐条 Put 准备 Snapshot 数据。运行约一小时后，WAL 增长与源码检查
证明 setup 正遭受上述 O(N²) 成本，预计完整运行需数十小时；该进程被明确终止，目录在
VM 中保留为 `20260909-aborted-quadratic-setup`，没有进入本报告。

正式保留的 v2 run 仅改变未计时 setup：Snapshot 和 overlap 的基础数据以每批最多
500,000 operations 写入；实际 Scan、Iterator、并发 5,000 Put，以及全部 Group Commit
计时 workload 均未改变。本地 10k smoke 后重新制作源码归档，再完成两轮 Linux 运行。

## 当前项目性能状态

当前证据支持以下有边界的简历或项目表述：

- “profile-guided decoded-block cache improved representative fixed-Linux SSTable hit
  throughput from about 10k to 1.36M ops/s”；
- “simplified size-tiered compaction nearly doubled the defined mixed-workload throughput
  while reducing point-read amplification from about 16.4 to 2.14, at 5.54x higher write
  amplification”；
- “SSTable v2 reduced shared-prefix block size by up to 27.7% and improved representative
  block lookup by about 42%”；
- “bounded group commit improved 8/16-writer synchronous throughput by 2.58–2.59x and
  4.37–4.49x versus the same queue with grouping disabled”；
- “replacing full-MemTable batch copies with touched-key staging restored the affected
  writer-overlap workload from about 0.7k to 5.1–6.0M ops/s”。

单操作写入回归已经按最小闭环完成：修复 touched-key 原子暂存，执行相关 unit/boundary
正确性测试，并在固定 Linux VM 两轮复测全部 12 个受影响 case。未重复不受此次修改影响的
Snapshot GC、SSTable codec 全矩阵，也未为了性能采集重复完整 Debug/ASan/TSan 套件。
