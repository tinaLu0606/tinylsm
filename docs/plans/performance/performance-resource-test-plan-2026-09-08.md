# TinyLSM 性能与资源实验测试计划

状态：`计划`
适用范围：当前 TinyLSM C++ 引擎；Lab 仅作为观察和正确性实验界面，不作为性能基准计时器。

## 1. 目的与结论边界

本计划的目标是让每次性能实验回答一个明确问题，并保留足以复现的证据：

```text
固定源码 + 固定环境 + 固定 workload
        + 多次重复 + 原始 JSON/日志
        = 可以比较的单项结论
```

它不把不同 workload 的 ops/s、RSS 和 p99 合成为“数据库总分”，也不把 Lab 的页面时间当成
引擎裸吞吐。Lab 在每次操作周围会读取状态、目录和指标，适合观察 WAL、MemTable、SST 与
Compaction 生命周期；严肃 benchmark 必须只计时 `DB::Put/Get/Scan/Compact` 的目标循环。

本文件只规划当前引擎的量化性能实验，目标是形成可以诚实写入简历的数字。未来 Snapshot/
MVCC、SSTable v2 和 Group Commit 的专项 before/after 与正确性验收，不混入本文件，统一放在
`docs/plans/next-engineering-goals.md` 的对应 Goal 中。

## 2. 已有基线与本次起点

已有报告已经建立了三个可复现的 Linux Release 基线：

| 主题 | 已有设计 | 当前用途 |
| --- | --- | --- |
| 读取 | 新鲜 DB、固定 key/value、5 次重复；MemTable/SST hit、multi-table miss、Scan、1/2/4/8 reader | 检查 Block Cache 与 shared-read 行为 |
| 写入 | 256 B value、256 KiB MemTable；async 100,000 Put、sync 20,000 Put；各 5 次重复 | 分开观察 WAL durability 代价与 bounded background flush |
| Compaction | 20,000 次 mixed operation、4,096 key、256 B value、64 KiB MemTable、cache 关闭、sync 关闭；Manual 与 SizeTiered 各 5 次 | 检查 table pressure、读/写/空间放大和前台 latency |

已有结果是特定 Linux VM、特定 revision 的历史证据，不是未来运行必须达到的绝对阈值。
本计划先复现 Goal 3，再仅在新的 workload 或 profile 指出瓶颈时扩大范围。

## 3. 当前设备资源快照

采集时间：2026-09-08（Asia/Singapore）。以下是本机的运行环境描述，不是性能结果。

| 资源 | macOS 主机 | 固定 Linux VM | 对实验的影响 |
| --- | --- | --- | --- |
| CPU | Apple M5，10 核（4 Performance + 6 Efficiency） | 4 vCPU | VM 的 CPU 上限决定 Linux benchmark 的并行度；不要把 macOS 与 VM 的数值混比 |
| 内存 | 24 GiB；采集时系统约 53% 可用 | 8 GiB 配置，既有环境记录约 7.7 GiB 可用 | MemTable、Block Cache、OS page cache 和 benchmark 临时数据共享内存；有内存压力时结果无效 |
| 磁盘 | APFS Data 卷 926 GiB，总可用约 732 GiB | 40 GiB ext4 虚拟磁盘 | WAL sync、SST flush、Compaction 都依赖它；VM 的 40 GiB 是 Linux 实验的真实容量上限 |
| 当前负载 | Lab server 空闲；VM 采集时约 0% CPU | `tinylsm-linux` 正在运行但空闲 | 可以开始实验；编译或 benchmark 时应接电源并关闭其他重负载任务 |

当前 Lab server 在 macOS 主机运行，Lab 数据目录是
`/Users/tina/workspace/tinylsm/var/lab/session-demo`，使用主机 APFS 空间，**不**使用
VM 的 40 GiB 磁盘。Linux benchmark 则必须明确记录 guest 的 ext4 挂载点。

## 4. 参考项目带来的设计原则

LevelDB 的 `db_bench` 将 fill、sync fill、随机/顺序读取、missing read、删除、读写并行和
compaction 切成独立 workload；其公开性能说明也固定 key/value/entry 数与机器信息。
RocksDB 的 `db_bench` 在此基础上提供 fill、overwrite、delete、random read、read while
writing、read while merging、seek 等可组合 workload。TinyLSM 借鉴的是这种“每个 workload
只回答一个问题”的方法，而不是复制它们复杂的选项或以其结果为性能目标。

- [LevelDB `db_bench` source](https://github.com/google/leveldb/blob/main/benchmarks/db_bench.cc)
- [LevelDB performance report](https://github.com/google/leveldb)
- [RocksDB benchmarking tools](https://github.com/facebook/rocksdb/wiki/Benchmarking-tools)
- [RocksDB `db_bench` implementation](https://github.com/facebook/rocksdb/blob/main/tools/db_bench_tool.cc)

## 5. 所有正式 benchmark 的共同契约

1. 使用干净、固定的 C++ source revision；若主工作区有 UI 或文档改动，使用 Git bundle
   或 clean clone，不把 dirty worktree 当作正式证据。
2. 每个 repetition 使用新的临时 DB；不得复用上一轮的 WAL、SST、page cache 状态。
3. Release 计时前完成 Debug、ASan/UBSan、TSan 和 Release build；性能结果不会代替正确性。
4. 每 case 至少 5 次重复，保存 Google Benchmark 的完整 JSON；报告 median 与 CV。
   CV >= 10% 时不宣布性能变化，先检查机器负载、样本长度和 cache/disk 状态后重跑。
5. 保存 source revision/status、OS/kernel、CPU、RAM、文件系统、磁盘剩余空间、编译器和
   CMake/Ninja/benchmark 版本。Linux 由脚本写入 `environment.txt`。
6. 只在同一 workload、同一 durability/cache/options、同一环境内比较。sync 与 async、
   macOS 与 Linux、Lab 与 benchmark 的时间都不能直接横向比较。
7. 需要解释瓶颈时，在固定 Linux VM 用 `perf cpu-clock` 采样；Lima/VZ 不支持可靠硬件
   PMU 事件，不能报告 cycles/cache-misses 为已测结果。

## 6. 执行顺序与测试矩阵

### P0：复现当前 Goal 3（下一次正式实验）

目的：确认当前 revision 的 automatic size-tiered compaction 在同一 Linux 环境仍可复现。

| 项目 | 固定值 |
| --- | --- |
| Case | `CompactionMixedManual`、`CompactionMixedSizeTiered` |
| workload | 20,000 operations；4,096 keys；256 B value；每 100 次含 98 Get、1 Delete、1 Scan，其余 Put |
| options | 64 KiB MemTable；Block Cache=0；`sync_on_write=false`；SizeTiered trigger=4 或 Manual=0 |
| 重复 | 每 case 5 次；另一次完整独立 rerun |
| 记录 | ops/s、Put/Get/Scan p50/p95/p99、read/write/space amplification、table/debt、Compaction 次数 |
| 有效性 | JSON verifier 通过，保留 case 的 CV < 10%，Debug/ASan/TSan/Release 都通过 |

在固定 Linux VM 的干净 checkout 中运行：

```sh
./scripts/run_linux_compaction_goal3.sh <output-dir>
```

复现与现有结果差异超过约 15% 且两次稳定运行都同向时，先 profile 或检查环境，不立即改
算法。这是调查触发线，不是 CI fail 门槛。

### P1：规模增长实验（P0 稳定后）

目的：回答“数据量变大后，size-tiered 的读收益、写放大和磁盘占用怎样变化”。

| 阶段 | 操作数 | key space | value | 开始条件 |
| --- | ---: | ---: | ---: | --- |
| S1 | 100,000 | 65,536 | 256 B | P0 有效 |
| S2 | 1,000,000 | 262,144 | 256 B | S1 的 CV < 10%，VM 磁盘与内存没有压力 |

每个阶段都比较 Manual 与 SizeTiered；保持相同随机种子、操作 mix 和 options，只改变规模。
S2 约产生 256 MB 逻辑写入量，远低于 VM 40 GiB 磁盘，但 Compaction 临时空间、运行时和
page cache 仍必须记录。任何一轮出现磁盘低余量、OOM、明显 swap 或 CV >= 10%，停止扩大。

### P2：读路径与并发读回归

目的：确保 Compaction 或后续改动没有破坏已验证的读取路径。

| workload | 控制变量 | 观察指标 |
| --- | --- | --- |
| MemTable/SST hit、multi-table miss | cache 关/开 | ops/s、table probes、decode、cache hit/miss |
| repeated hot read | 256 个热 key | cache charge、LRU hit rate、ops/s |
| Scan | 单表与多表、固定返回条数 | Scan latency、block decode、table inputs |
| concurrent read | 1/2/4/8 reader，只读 | 总吞吐与每 reader 效率；不承诺线性加速 |

执行 `scripts/run_linux_read_goal1.sh <output-dir>`；只有 cache/probe 指标显示瓶颈变化时才
考虑 Bloom Filter 或更复杂缓存策略。

### P3：写入 durability 与后台 flush 回归

目的：持续区分“快但可丢最近确认写”的 async 与“每次确认都同步 WAL”的 sync。

| case | 操作数 | 必须记录 |
| --- | ---: | --- |
| async flush | 100,000 Put | ops/s、Put p50/p95/p99、flush stall、rotation/flush 次数、queue depth |
| sync WAL | 20,000 Put | 同上，另加 `wal_syncs` |

执行 `scripts/run_linux_write_goal2.sh <output-dir>`。只有多 writer profile 明确显示 sync 或
queue contention 是主成本时，才讨论 Group Commit；单线程 async/sync 差异不能证明它值得实现。

### P4：恢复与 Lab（非性能 gate）

- Lab：小批量 Put/Delete/Get/Scan，观察 WAL、MemTable、SST、Manifest 和 recovery sandbox。
- C++：保持故障注入、Close/Reopen、WAL tail truncate、Manifest CRC、background worker
  failure 的 Debug/ASan/TSan 覆盖。
- 通过标准是数据/状态正确与错误边界可解释，不报告 Lab 的 duration 为引擎 p99。

## 7. 每次实验的输出与报告模板

输出目录应包含：

```text
<goal>-<platform>-<date>-runN/
├── environment.txt
├── debug-test.log
├── asan-ubsan-test.log
├── tsan-test.log
├── release-build.log
├── benchmark.log
├── benchmark.json
└── verification.log
```

结果报告必须回答：测试的问题是什么、固定了什么、改了什么、median/CV/raw counter 是什么、
哪些结论成立、哪些不能外推、下一步是否需要 profile。原始 JSON 是证据；Markdown 只做解释。

## 8. 当前不做的事情

- 不把性能实验接入 CI gate；先积累稳定的同环境结果。
- 不在没有 profile 的情况下实现 Bloom Filter、Group Commit 或 leveled compaction。
- 不将 Linux VM 的数值包装为这台 Mac 的通用性能，也不拿 TinyLSM 与 LevelDB/RocksDB 做
  不同 durability 或配置下的产品排名。
- 不用 Lab UI 的 ops/s、p99 或 Mock 数据作为正式 benchmark 证据。

## 9. 面向简历的三组量化实验

现有 P0-P4 是具体测试矩阵；本节把它们收束为三组可以对外解释的实验。每组必须独立完成
两轮 fixed-Linux Release 运行，保留原始 JSON，不能从不同 workload 中拼接最好数字。

### 实验 A：读取效率与并发扩展

要回答的问题：Block Cache 和 shared-read 让哪些读取更快，多 reader 能扩展到什么程度？

| 维度 | 固定对照 | 简历候选指标 |
| --- | --- | --- |
| 热点读取 | cache=0 与默认 8 MiB；同一 256-key working set | ops/s 提升倍数、cache hit rate、cache charge |
| 单表/多表读取 | 相同 key/value、表数和命中位置 | single/multi-table hit、negative miss、table probes |
| Scan | 相同返回条数；单表与多表 | entries/s、p50/p95/p99、block decodes、峰值 RSS |
| 并发读 | 固定 40,000 总操作；1/2/4/8 reader | 总吞吐、相对 1-reader speedup、parallel efficiency |

`parallel efficiency = N-reader speedup / N`。例如 4 reader 是单 reader 的 2 倍吞吐，
效率是 `2/4=50%`；它比只写“支持并发”更能说明扩展边界。

### 实验 B：写入吞吐与前台停顿

要回答的问题：bounded background flush 在保持内存有界时，减少了多少前台 stall？

| 维度 | 固定对照 | 简历候选指标 |
| --- | --- | --- |
| async write | 固定 100,000 Put、value 和 MemTable 大小 | ops/s、Put p95/p99、rotation/flush 次数 |
| sync write | 固定 20,000 Put、`sync_on_write=true` | ops/s、p95/p99、`wal_syncs` |
| backpressure | 小 MemTable、同一写入量 | wait 次数、累计等待、最长/高分位 stall |
| 资源 | 同一 workload wrapper | peak RSS、最大 immutable bytes/queue depth、输出 SST bytes |

简历结论必须把 async 与 sync 分开。async 的高吞吐不能表述成“每次写入都已落盘”；sync
的吞吐才对应每次确认前 WAL 同步的语义。

### 实验 C：Compaction 的收益与代价

要回答的问题：后台 partial compaction 用多少额外写入，换来多少 table/read pressure 降低？

| 维度 | 固定对照 | 简历候选指标 |
| --- | --- | --- |
| 策略 | Manual 与 SizeTiered；同 seed/mix/options | point-read amplification、table count/debt |
| 代价 | 同 logical writes/live bytes | write amplification、space amplification |
| 前台体验 | 同 20,000 mixed operations | Put/Get/Scan p95/p99、总 ops/s |
| 规模曲线 | P0、S1、S2 | 指标随 20k/100k/1m operations 的变化 |

Compaction 不应只宣传吞吐提升。若 read amplification 降低但 write amplification 上升，
两者必须一起报告，这正是 LSM 策略真正的工程权衡。

## 10. 可选的同条件 LevelDB 对照

LevelDB 对照适合提供参照系，但不是必须完成的产品排名。只有以下条件都能对齐时才写入
正式报告：

- 相同 Linux VM、guest-local ext4、编译模式、key/value、操作数和线程数；
- compression 同为关闭或明确分别报告；Block Cache 预算一致；
- sync/async durability 含义一致；数据库都经过相同填充、flush/compaction 预处理；
- 分开比较 Put、Get、missing Get 和 Scan，不使用一个综合分数；
- 同样报告两轮 median/CV，不能只选择 TinyLSM 最有利的 case。

LevelDB 更快或更慢都不是失败；有价值的是能解释差异来自格式、cache、compaction、锁还是
durability 配置。

## 11. 简历数字的准入规则与交付物

一个数字只有同时满足以下条件，才进入最终简历候选表：

1. fixed-Linux 两次独立运行方向一致，目标 case CV < 10%；
2. workload、数据规模、durability、cache 和线程数可以用一句话说清；
3. raw JSON、environment、revision 和验证脚本都已保存；
4. 提升同时给出 before、after 和代价指标，不只给百分比；
5. 没有把微基准外推成生产数据库能力。

最终新增一份 `reports/performance/portfolio-benchmark-YYYY-MM-DD.md`，包含：

```text
一句话结论
-> workload 与环境
-> 两轮 median/CV
-> raw JSON 链接
-> 代价与适用边界
-> 可直接改写成简历 bullet 的中文/英文候选句
```

推荐最终只选 2-3 个最有解释力的数字：读取 cache 收益与并发扩展、background flush 的
尾延迟/背压、partial compaction 的 read-vs-write amplification 权衡。数字少但证据完整，
比罗列十几个 ops/s 更可信。
