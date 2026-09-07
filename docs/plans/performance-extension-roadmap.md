# TinyLSM Performance Extension Roadmap

## 文档状态

- 日期：2026-09-07
- 状态：`Goal 1 已完成；Goal 2/3 待执行`
- 基线提交：`16ed69f feat: finalize atomic batches and performance baseline`
- 当前主仓库提交：`89813e7 fix: update TinyLSM Lab frontend`
- 基线报告：`docs/performance/baseline-2026-09-06.md`
- 定位：将性能扩展划分为三个可由 Codex 一次 goal 完成的大阶段

原初版功能已经收口。后续路线不再拆成大量小 ticket，而是让一个 Codex goal
负责一个完整性能主题：测量、设计、实现、故障验证、前后对比、文档和提交都在
同一次 goal 中闭环。goal 内部可以有多个 checkpoint commit。

## 1. 所有 Goal 的执行合同

### 开始前

1. 阅读根目录 `AGENTS.md`、本路线、基线报告和最近 devlog。
2. 检查主仓库、`tools/lab_web` 子模块、分支、远端和已有 dirty 文件。
3. 不覆盖、不暂存、不提交与当前 goal 无关的用户修改。
4. 复现相关正确性测试与性能起点，确认 benchmark 的计时边界和方差。

### 执行方式

- 先用 benchmark 和 profiler 确认瓶颈，再选择实现；不为完成计划强行加入某项
  优化。
- 一个 goal 可以连续完成 benchmark 改进、核心实现、并发/恢复测试和报告，
  但每个关键架构变化都要有独立 checkpoint。
- profile 推翻原假设时，改为解决实际热点，并在 devlog 记录原假设为何废弃。
- 性能提升不能依赖弱化 durability、取消校验或无界占用内存。
- Lab timing 不作为核心引擎性能数据。

### Commit、devlog 与 push

用户已授权执行这些 goal 时自行：

- 使用 path-scoped `git add` 和普通 `git commit` 建立关键 checkpoint；
- 更新当天 `docs/devlog/YYYY-MM-DD.md`；
- 在 README、设计文档和性能报告中同步真实实现边界。

不得自行 amend、rebase、reset、切换分支或处理无关 dirty 文件。不得自行 push。
每个 goal 完成后必须列出 commits，并明确提醒用户决定是否 push。

### 通用回归

每个实现 goal 至少完成：

```sh
./run format --check
./run test
./run asan
./run tsan
./run release
git diff --check
```

涉及持久化格式、flush 或 compaction 时，还必须覆盖旧格式兼容、reopen、损坏、
截断和故障注入。涉及性能结论时，必须在固定 Linux VM 保存至少两轮 Release
原始 JSON，并记录环境、median、CV 和代价。目标 workload 的 CV 应尽量低于
10%；不稳定的数据不能作为优化通过门槛。

## 2. 三个 Goal 总览

```text
Goal 1：读取性能
benchmark + profile + cache/filter + 并发读
                    |
                    v
Goal 2：异步写入
多 WAL + immutable MemTable + background flush + 可选 group commit
                    |
                    v
Goal 3：Compaction 演进
指标 + partial compaction + background compaction + 综合报告
```

三个 goal 顺序执行。每个 goal 完成后都重新评估下一阶段，不因路线文档存在就
自动继续。

## 3. Goal 1：读取路径性能扩展

### 可直接创建的 Goal 目标

> 完成 TinyLSM 读取路径性能扩展：稳定和细分读取 benchmark，使用 Linux profile
> 定位随机命中读与 Scan 的主要成本；根据证据实现有界 Block Cache 或实际热点
> 优化，仅在 negative lookup 数据支持时加入 Bloom Filter；随后缩小只读操作的
> 锁粒度，使安全的 Get/Scan 可以并行。完成正确性、损坏读取、内存边界、并发、
> sanitizer 和固定 Linux before/after 验收，提交关键 checkpoints、更新 devlog
> 与性能报告；不 push，结束时提醒用户。

### Goal 内部阶段

#### A. 测量与定位

- 拆分 MemTable hit、单 SSTable hit、多 SSTable hit、warm repeated hit、首次
  cold hit、negative lookup 和重复 Scan。
- 延长当前单次 Scan，降低 `38.28%` CV。
- 修正并发 benchmark：固定总操作数、数据规模和 flush 次数。
- 增加 block read/decode、table probe、cache 和锁等待的最小必要计数。
- 在 Linux 采集 CPU profile，写清主要热点调用链和占比。

#### B. 读取热点优化

- 若 profile 证实重复 block 读取/解码占主导，实现容量有上限的 LRU Block
  Cache；否则优化 profile 证明的实际热点，并记录为何没有做 cache。
- Block Cache 使用 SSTable 身份与 block offset 作为 key，拥有缓存数据，只缓存
  通过 checksum/结构验证的 block。
- Options 支持配置容量和关闭缓存；记录 hit、miss、insert、eviction、charge。
- Reader 销毁、compaction 和 file number 生命周期不能产生 stale read。
- Bloom Filter 是条件项：只有多 SSTable miss 的 table/block probe 明显时才做；
  必须无 false negative，并保持旧 SSTable 读取兼容。

#### C. 并发读

- 明确 Get、Scan、diagnostic read、write、Compact 和 Close 的共享/独占边界。
- 可评估 `std::shared_mutex`，但必须处理 Scan 长时间持锁和 cache 锁顺序。
- 测试证明读者确实可以重叠；writer、Compact、Close 的等待与可见性确定。

### 完成标准

- 新 benchmark 能区分命中层级，主要 case 两轮 Linux 结果可复现。
- 优化在目标 workload 上有稳定正收益，同时报告 RSS、cache charge 和回归项。
- disabled cache/filter 保持原行为；损坏 block 不能因缓存绕过校验。
- 无 stale read、悬空 Reader、死锁或部分 Scan；ASan/UBSan、TSan 全套通过。
- 报告 1/2/4/8 readers 的吞吐与扩展效率。
- README、设计文档、原始 JSON、性能报告和 devlog 已更新并提交。
- goal 结束时工作区除启动时记录的无关 dirty 外保持干净，并提醒用户 push。

### 建议 Checkpoints

1. 读取 benchmark 与 profile 报告；
2. cache/filter 或实际热点优化；
3. 并发读与 TSan 验收；
4. Linux before/after、README 和 devlog 收口。

### 2026-09-07 完成记录

- 目标 benchmark、两轮 fixed-Linux 原始 JSON、profile、Block Cache 与 shared
  read-lock 验收已完成；详见
  `docs/performance/read-path-2026-09-07.md`。
- Bloom Filter 因 cache 后 negative lookup 已消除重复 decode/CRC 热点而暂不加入。
- Goal 2 仍需用户根据本结果另行决定是否创建和执行。

## 4. Goal 2：异步写入路径扩展

### 前置条件

- Goal 1 已提交，读取状态和锁边界稳定。

### 可直接创建的 Goal 目标

> 完成 TinyLSM 异步写入路径扩展：先测量 flush stall、写入尾延迟和并发 writer
> 成本；扩展 Manifest/WAL 恢复以支持 active 与 immutable WAL；实现一个有界
> immutable MemTable 和单 background flush worker，完整处理背压、Close、后台
> 错误和 crash recovery；若 profile 证明 sync 合并有价值，再实现有界 writer
> queue/group commit。完成格式兼容、故障矩阵、并发、sanitizer 和固定 Linux
> before/after 验收，关键点自行 commit 并更新 devlog；不 push，结束时提醒用户。

### Goal 内部阶段

#### A. 写入测量

- 增加 flush stall、p50/p95/p99、WAL sync、后台队列深度和吞吐指标。
- 固定总操作数、value 大小、MemTable threshold 和 sync 语义。

#### B. 多 WAL 与恢复基础

- Manifest 描述一个 active WAL 和有界 immutable WAL，并使用新版本编码。
- 保持现有 Manifest 与 version-1/version-2 WAL 读取兼容。
- 定义 WAL 顺序、sequence、published floor、重复/缺失 WAL 和 cleanup 规则。
- 先独立完成恢复与故障矩阵 checkpoint，再加入线程。

#### C. Immutable MemTable 与后台 Flush

- active MemTable/WAL 和 immutable MemTable/WAL 一一对应。
- 第一版最多一个 immutable；再次填满时前台等待，形成有界背压。
- 后台失败保存为 sticky error，后续操作和 Close 的返回语义统一。
- Manifest durable 后才能释放旧 WAL/MemTable；cleanup 失败不反转成功提交。
- Close 停止接收新操作，唤醒等待者并安全结束 worker。

#### D. 条件项：Group Commit

- 只有 profile 显示 sync 或 writer 竞争仍是主要成本时才加入。
- writer queue 必须有界；leader 可合并兼容请求，但保持每个 WriteBatch 的原子
  边界、sequence 和返回状态。
- append/sync 失败时，每个等待者得到与 durable 状态一致的结果。

### 完成标准

- crash/reopen 不丢失已确认写，也不暴露半提交 batch 或半 flush 状态。
- 故障注入覆盖 WAL rotate、SST build、Manifest rename/SyncDir、cleanup 和 Close。
- 无后台线程泄漏、死锁、无限等待或无界内存；ASan/UBSan、TSan 全套通过。
- Linux 报告同步/异步写吞吐、p95/p99、stall time、RSS 和失败边界。
- README、格式文档、原始 JSON、性能报告和 devlog 已更新并提交。
- goal 结束时提醒用户 push。

### 建议 Checkpoints

1. 写入 benchmark、指标与状态设计；
2. 多 WAL/Manifest 格式和恢复兼容；
3. background flush、背压和错误传播；
4. 可选 group commit；
5. Linux before/after 与文档收口。

## 5. Goal 3：Compaction 与综合性能扩展

### 前置条件

- Goal 2 已提交，后台 worker 和多版本读取生命周期稳定。

### 可直接创建的 Goal 目标

> 完成 TinyLSM Compaction 性能扩展：建立 read/write/space amplification、table
> count、compaction debt 和前台尾延迟指标；基于实验在 size-tiered 与简化
> leveled 中选择最小策略，实现正确保留 tombstone 的 partial compaction，再将
> 已验证策略放入有界 background compaction worker；完成并发版本可见性、
> Manifest 故障恢复、长时间 mixed workload、sanitizer 和固定 Linux 综合验收，
> 输出最终性能报告并提交关键 checkpoints；不 push，结束时提醒用户。

### Goal 内部阶段

#### A. 指标与策略决策

- workload 覆盖顺序覆盖写、热点覆盖、删除、多表 point read 和 range scan。
- 指标可以由原始计数复算，不能只输出综合分数。
- 明确选择 size-tiered 或简化 leveled，并记录拒绝另一方案的原因。
- 在实现前写清输入选择、输出规则、tombstone 删除条件和 Manifest commit point。

#### B. Partial Compaction

- 只合并策略选择的 SSTable，未参与表继续可见。
- 只有能证明覆盖所有更旧版本时才允许删除 tombstone。
- 覆盖 0/1/N 输入、输出为空、重复 key、构建/读取失败和完整 Manifest 故障点。
- 若需要 VersionEdit/VersionSet，必须作为独立 checkpoint 明确实现，不能隐式
  混入后台线程。

#### C. Background Compaction

- 使用不可变 table/version 所有权支撑并发读，不能产生悬空 iterator。
- flush 与 compaction 的 Manifest 更新具有统一串行化点，不能 lost update。
- 调度有界、无 busy loop；Close、取消、后台错误和 cleanup 行为确定。

#### D. 综合验收

- 运行长时间 mixed read/write/delete/scan workload 和 crash/reopen 验证。
- 从干净 checkout 复现构建、tests、sanitizers、Linux benchmark 和部署 smoke。
- 汇总 Goal 1-3 的 throughput、p50/p95/p99、RSS、read/write/space
  amplification 和 CV；未知项明确标记。

### 完成标准

- partial/background compaction 前后 Get/Scan 逻辑结果一致。
- tombstone 不会使旧值复活；Manifest publication 与 cleanup 边界保持可靠。
- 无 lost update、悬空 reader、线程泄漏或后台 busy loop；全部回归通过。
- 相比同步 full compaction，table count、amplification 和前台尾延迟有可解释结果。
- README、设计文档、原始数据、最终性能报告和 devlog 已更新并提交。
- goal 结束时工作区干净，列出全部 commits，并提醒用户 push。

### 建议 Checkpoints

1. 指标、workload 和 compaction 策略决定；
2. partial compaction 与恢复故障矩阵；
3. background compaction 与并发版本生命周期；
4. 综合 Linux 验收、最终报告和 devlog。

## 6. 当前执行建议

现在只创建并执行 **Goal 1：读取路径性能扩展**。

它一次完成 benchmark 稳定化、profile、读取热点优化、条件式 Bloom Filter、并发
读、sanitizer、Linux before/after、commits 和 devlog，范围足够形成完整成果，
同时不会提前改变 WAL/Manifest 和后台线程模型。

Goal 1 完成后查看真实结果，再决定是否创建 Goal 2；不提前把三个 goal 连续运行。
