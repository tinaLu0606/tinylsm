# TinyLSM 下一阶段工程 Goal 计划

## 文档状态

- 日期：2026-09-08
- 性质：Goal 4/5/6 的功能实现记录与验收边界；固定 Linux 性能实验仍待单独安排
- 输入：`post-performance-learning-roadmap.md` 的同类项目调研与讨论结论
- 验证边界：本计划只保留证明新增语义、持久化边界和并发安全所必需的测试；不因“覆盖率”
  重复枚举等价组合。性能压测另行安排，不是每个工程阶段的阻塞项；
  `test-plan-2026-09-08.md` 继续只服务当前引擎的简历量化实验

## 1. 为什么只分三个大型 Goal

当前 TinyLSM 已经完成基本 LSM 生命周期、Block Cache、后台 Flush 和后台 partial
compaction。下一阶段选择三个能形成完整学习闭环的主题：

```text
Goal 4：一致性读取
Iterator + Snapshot + MVCC + 旧版本回收
                       ↓
Goal 5：SSTable v2
压缩格式 + 随机定位 + metadata + 兼容
                       ↓
Goal 6：高并发持久化写
multiwriter profile + 条件式 Group Commit
```

三个 Goal 都要求“设计、实现、关键风险验证、文档和提交”在同一 Goal 中完成。性能量化在
功能稳定后按单独计划安排，不阻塞日常开发。Model-based、crash 和 fuzz testing 不单独变成
第四条功能线，只在它们能覆盖普通单测无法覆盖的风险时放进对应 Goal。

## 2. 所有 Goal 的执行合同

- 开始前检查当前源码、测试、主仓库/子模块状态和已有 dirty 文件，不覆盖无关修改。
- 先用小例子写清语义、不变量、所有权、锁顺序和 durable commit point，再实现。
- 涉及格式时保留旧版本读取兼容；每类独立风险只选一个最小用例验证，例如正常读取、截断、
  校验和损坏和长度上限，而不对同一失败原因做组合穷举。
- 日常改动只跑受影响的单测和一个端到端集成用例。Goal 完成时跑一次 Debug 回归；只有改到
  内存所有权时才补 ASan/UBSan，改到并发同步时才补 TSan，改到 durable commit point 时才补
  对应 fault injection/reopen。Release 只用于发布或单独的性能工作。
- 性能结论在用户单独安排的固定 Linux 实验中产出；需要正式结论时才采集两轮 Release 原始
  JSON、median/CV 和资源代价。
- 同一行为已有更低层测试覆盖时，不再为不同数据规模、相同调用顺序或相同错误传播路径新增
  重复测试；优先维护少量、可读且能定位失败原因的测试。
- 每个关键架构阶段在用户明确授权后使用 path-scoped checkpoint commit；更新 README、设计
  文档和 devlog；不自行 push。
- 条件项若没有数据支持，可以以“测量后明确不实现”完成，不能为了完成列表强行加代码。

## 3. Goal 4：Public Iterator、Snapshot 与 MVCC

### Goal 目标

让调用者可以逐条扫描，并在写入、Flush 和 Compaction 继续发生时读取同一个时间点的
一致视图；使用 watermark 安全回收不再被快照需要的旧版本。

### 核心语义

```text
name@105 = Bob
name@101 = Alice
name@100 = Tina

Snapshot(102) -> Alice
最新读取       -> Bob
```

Snapshot 只记录逻辑 sequence，不复制整个数据库。MVCC 让同一个 user key 暂时保留多个
sequence 版本；watermark 是当前最老活跃 Snapshot 的 sequence，Compaction 以它判断哪些
旧版本和 tombstone 已经无人可见。

### 工程阶段

1. **Public Iterator**：提供 `Seek/Valid/key/value/Next/status`，逐条返回数据；定义
   Iterator 对 MemTable snapshot 和 SSTable Reader 的所有权，避免长 Scan 必须一直持有
   DB shared lock。
2. **MVCC internal key**：排序键变为 `(user_key, sequence descending)`；MemTable、SSTable、
   Block Cache、Get 和 merge iterator 支持同 key 多版本及 `sequence <= snapshot` 过滤。
3. **Snapshot API**：创建/释放只读 Snapshot；Get/Iterator 接受 Snapshot；明确 Snapshot、
   Iterator 与 DB Close 的生命周期合同。
4. **Watermark 与 GC**：跟踪最老活跃 Snapshot；Compaction 为每个 key 保留 watermark 可见
   版本，只删除已证明无人需要的更旧版本/tombstone。
5. **兼容与恢复**：决定 SSTable/Manifest 是否升版；旧数据库能读取或通过显式迁移进入
   新格式；内存 Snapshot 不跨进程恢复。

### 本 Goal 内的验证与实验

**语义与模型测试**：用一个固定 seed 的参考模型覆盖 Put/Delete/WriteBatch、Get 和
Iterator 的版本可见性；失败时保留可重放 trace。确定性的边界用例补充 Snapshot 创建、释放
和同 key 覆盖写，不再为等价操作排列重复生成测试。

**并发测试**：用一个集成场景在 Snapshot 存活期间交错 writer、MemTable rotation、
background flush 和 compaction；验证旧视图不变、新读者看见新值、Reader 不悬空。该场景
改动并发同步时用 TSan 运行一次，不为每种时序单独建用例。

**故障测试**：只在持久化提交边界选择代表性注入点（WAL replay 或 Manifest publication），
确认已确认写不丢、旧快照只存在于进程内、reopen 恢复最新持久化状态；不重复覆盖同一错误
传播路径的每个内部步骤。

**后续性能实验（单独安排，不阻塞本 Goal）**：

| 问题 | before/after 或变量 | 指标 |
| --- | --- | --- |
| Iterator 是否降低大 Scan 资源 | 现有 materialized Scan vs Iterator；10k/100k/1m entries | 首条结果延迟、完整扫描时间、peak RSS |
| 是否减少 writer 阻塞 | writer 与长 Scan/Iterator 并发 | writer p95/p99、shared-lock wait |
| Snapshot 保留代价 | 覆盖写率固定；Snapshot 保持 0/10/60 秒或等价操作数 | retained versions/bytes、live SST bytes、space amplification |
| GC 是否生效 | 释放最老 Snapshot 前后各触发 Compaction | reclaimed bytes/versions、结果正确性 |

Snapshot 存储代价不得只写百分比，必须给出近似关系：`覆盖写速度 × 平均记录大小 × 最老
Snapshot 年龄`，并报告实际 retained bytes。

### 完成标准

- Iterator 是 pull-based（调用者调用 `Next()` 才前进），不暗示内部自动创建线程。
- Snapshot 跨 MemTable、Flush 和 Compaction 保持一致；释放后旧版本可回收。
- 长期 Snapshot 的内存/磁盘增长有计数、有上限策略或明确告警边界。
- 受影响单测、一个端到端 Snapshot 生命周期场景，以及代表性恢复场景通过；涉及并发改动时
  补一次 TSan，README/API/格式文档同步。性能实验不作为本 Goal 的完成条件。

### 本 Goal 不做

不做 read-write transaction、冲突检测或 serializable isolation。先把只读版本可见性和
回收做正确。

## 4. Goal 5：SSTable v2 编码、索引与启动元信息

### Goal 目标

设计一个可版本化、可校验、兼容旧表的 SSTable v2，在真实 key 分布上减少存储空间，
同时控制随机 Get、Scan、构建和 Open 的 CPU/I/O 代价。

### 工程阶段

1. **后续测量起点（单独安排）**：增加 SSTable data/index/properties/footer 分区字节计数；
   建立共同前缀、随机 key、短 key、长 key 四类数据集。它用于解释空间/CPU 代价，不阻塞
   v2 格式、兼容与损坏处理的功能验收。
2. **Prefix compression**：记录与 restart key 的共享前缀长度和剩余后缀；对长度、边界、
   非法 overlap 和 truncated entry 做完整校验。
3. **Restart points**：每隔可配置 N 条保存完整 key 和 restart offset，使随机查找先定位
   附近 restart，再小范围重建 key；比较 N 对压缩率和 Get CPU 的影响。
4. **Properties/index metadata**：以 checksum 保护 entry count、key/sequence range 和各
   section offset；明确快速 Open 能验证什么，不能用 metadata 代替必要的数据损坏检查。
5. **条件式 Bloom Filter**：先建立超出 Block Cache 的冷数据、高 miss-ratio、多 SSTable
   workload。只有 table/block I/O 仍是主要成本时才加入；否则记录延期结论。
6. **兼容**：reader 自动识别 v1/v2；保留固定 golden files；compaction 可以作为逐步重写
   v1 表的迁移路径，不做原地修改。

### 本 Goal 内的验证与实验

**格式测试**：每个 section 选一个 round-trip/golden 用例，再分别选一个截断、校验和损坏和
长度上限用例；这些用例共同证明 decoder 只会成功或受控失败。若 decoder 解析逻辑复杂，再以
一个共享 fuzz target 补充随机输入，不为每个 section 维护重复 fuzz target。

**兼容测试**：覆盖旧 v1 reopen、一个 v1/v2 混合读取/Compaction 场景和 v2 输出 reopen；
任何读取错误不得返回部分 Scan。

**后续性能实验（单独安排，不阻塞本 Goal）**：

| 数据集 | 对照 | 必须记录 |
| --- | --- | --- |
| 高共同前缀 key | v1 vs v2、多个 restart interval | SST bytes/entry、压缩率、build MB/s、Get/Scan p95/p99 |
| 随机 key | v1 vs v2 | 空间收益是否仍覆盖额外 CPU |
| 大于 cache 的冷读 | cache=0/固定预算 | block reads/decodes、Get miss/hit latency |
| 多 SSTable negative lookup | Bloom off/候选 on，仅在触发时 | false-positive rate、跳过表数、额外 filter bytes |
| 大数据库 Open | v1/v2、相同 live bytes/table count | Open p50/p95、实际读取字节、验证覆盖范围 |

现有 `live_sstable_bytes`、`logical_live_bytes`、space/write amplification 继续使用，但还要
新增 section bytes、bytes/entry 和 build/decode CPU，才能解释“省了多少空间，付出多少
计算”。

当前已准备 data-block v1/v2 的 `bytes_per_entry`、编码和随机 point-lookup CPU；完整
section bytes 与 whole-DB Open 仍只在固定 Linux 的独立实验中解释，不将微基准外推为它们。

### 完成标准

- v1/v2 混合读取、Compaction、代表性 corruption 和 reopen 通过；解析逻辑改动时补一次
  ASan/UBSan，复杂 decoder 再运行共享 fuzz target。
- Bloom 没有 false negative；若未实现，报告数据为何不支持。

### 本 Goal 不做

不顺带引入完整 leveled compaction、压缩库矩阵或远程 SSTable。

## 5. Goal 6：高并发同步写与条件式 Group Commit

### Goal 目标

实现 bounded writer queue 和 Group Commit，并保持每个 WriteBatch 的原子边界、sequence
顺序和恢复语义。多 writer 的同步 WAL 是否由重复 `fsync` 和 writer serialization 主导，
以及实现是否降低同步次数，留给单独安排的性能实验判断。

### 工程阶段

1. **后续 multiwriter benchmark/profile（单独安排）**：1/2/4/8/16 writer，固定总操作数、
   value、MemTable、sync 语义和 flush 条件；记录 lock wait、queue wait、WAL append/sync
   时间。它不阻塞本 Goal 的语义实现与验证。
2. **性能决策门（后续单独安排）**：在 `sync_on_write=true` 下检查 WAL sync/queue
   contention 是否主导、multiwriter 是否无稳定扩展；async 或单 writer 数据不能作为
   Group Commit 性能收益的依据。当前 Goal 仅实现并验证语义，不因此宣称吞吐收益。
3. **bounded writer queue**：请求拥有完成状态；一个 leader 选择有界 group、分配连续
   sequence、编码 WAL，followers 等待各自结果；队列限制请求数/字节并有背压。
4. **durability 与错误广播**：完整 group append 并按请求要求 sync 后才能确认；partial
   tail 恢复时整体丢弃未完成 group；append/sync 结果不确定时，同 group 请求得到一致且
  保守的状态，必要时 handle 进入 terminal error 后 reopen。
5. **应用 MemTable**：WAL 成功后按 sequence 应用各用户 WriteBatch，任何读者不能看到一个
   WriteBatch 的一半；明确 group 只是一次 I/O 合并，不把多个调用者变成一个事务。

### 本 Goal 内的验证与实验

**并发模型测试**：用一个固定的多 writer trace 保存 Put/Delete/WriteBatch 的调用、返回和
sequence 区间；验证历史可解释为符合实时先后的串行顺序。只在新增队列或唤醒路径时扩展
trace，不按 writer 数量复制相同语义测试。

**恢复/故障测试**：选择 WAL append/sync 失败和 Close 唤醒两个代表性边界；reopen 对照已
确认集合，不完整 group 不得出现部分用户 batch。内部阶段若走同一错误传播路径，不再逐点
复制测试。

**后续性能实验（单独安排，不阻塞本 Goal）**：

| 变量 | 指标 |
| --- | --- |
| 1/2/4/8/16 writer，固定总 sync writes | ops/s、p50/p95/p99、相对单 writer speedup |
| Group Commit off/on | `wal_syncs / writes`、平均/p95 group size、leader/queue wait |
| queue 请求数/字节上限 | backpressure 次数/时间、peak RSS |
| 小/中 WriteBatch 混合 | batch 原子性、吞吐、公平性、最大等待时间 |
| fault/reopen run | acknowledged/lost/ambiguous 数量及其合同解释 |

当前已准备固定总 8,192 个 sync writes 的 1/2/4/8/16 writer、group off/on 对照；off 只将
group 上限设为 1，仍保留同一 bounded queue。结果须以两轮 Linux raw JSON 的 CV 为准。

### 完成标准

- 一个 writer/group 的错误不会造成其他请求永久等待；Close 无死锁、线程泄漏或遗漏唤醒。
- 所有已确认 sync 写在 fault contract 下可恢复；用户 WriteBatch 不会半可见。
- 新增并发队列时跑一次 TSan；涉及缓冲区/所有权时跑一次 ASan/UBSan；代表性故障用例通过。
- 若决策门不成立，以轻量 profile 记录不实现原因完成，不转做无关优化；正式吞吐/尾延迟结论
  留给单独安排的 Linux 性能实验。

### 本 Goal 不做

不做通用事务、两阶段提交或跨进程复制。Group Commit 只优化同一 DB 内多个 WAL 写入的
物理提交，不改变单个 WriteBatch 的公开原子性语义。

## 6. 推荐执行顺序与停止点

```text
先执行 Goal 4
    ↓ 完成后重新评估数据格式和版本保留成本
再执行 Goal 5
    ↓ 完成后用新格式重新建立 multiwriter 起点
最后决定是否执行 Goal 6
```

每个 Goal 结束后都停下来阅读真实结果，不因本文存在就自动继续。若只选择一个，Goal 4
仍是首选；它对版本可见性、并发和资源生命周期的学习密度最高。

## 7. 预期文档产物

每个 Goal 至少新增或更新：

```text
docs/design/<topic>.md                 设计与不变量
reports/performance/<topic>-YYYY-MM-DD.md 实验方法、原始数据索引与结论
docs/devlog/YYYY-MM-DD.md              实现过程和决策
README.md                              公开 API、数据流和真实限制
```

调研背景与术语解释继续保留在 `post-performance-learning-roadmap.md`；本文件只负责可以直接
创建工程 Goal 的边界、阶段和验收。
