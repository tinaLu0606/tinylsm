# 2026-08-19 · TinyLSM · 候选项目初步规划与评估

# 请先读：本文档的定位

本文档记录的是一个**仍在学习、比较和评估中的候选项目方案**，不是已经决定开工的需求书，也不是已经完成的项目说明。

考虑 TinyLSM 的目的，是判断它是否适合作为一个面向 C++/Linux 求职的个人项目：一方面系统训练存储引擎、持久化与恢复、文件格式、测试和性能分析等能力；另一方面在真正完成并取得可验证结果后，把这些能力作为简历项目展示。当前阶段的重点是先理解项目会做什么、为什么值得做、风险和工作量是否合适，再决定是否实施。

因此，后续 AI 在阅读本文档时应默认以下前提：

- TinyLSM 当前是较中意的候选方向，**尚未最终立项，也尚未实现或验证**；
- 项目服务于学习、能力提升和求职展示，但不能为了“像简历项目”而堆砌功能；
- 只有实际完成、测试或测量过的内容，才可以写成简历成果；计划、设想和参考资料不能表述成已实现能力；
- 评估标准包括：能否深入提升 C++/Linux 系统开发能力，能否与已有开发经历形成衔接并增加新的系统深度，以及范围能否由个人可靠完成；
- AI 应帮助梳理问题、解释源码和评审设计，但关键设计选择、实现、测试和性能结论需要由项目作者理解并负责。

# 记录来源

- 类型：阶段计划 / AI 讨论成果
- 日期：2026-08-19
- 最近更新：2026-08-21
- 来源：用户与 AI 关于 TinyLSM 项目方向、需求边界、规模、鲁棒性、扩展功能、开发环境与开源参考的讨论；开源项目说明以其官方仓库和文档为依据
- 状态：学习与评估中，未立项、未实现、未验证

# 文档地位

本文档是 TinyLSM 的第一份候选方案与讨论记录，用于评估和确认：

- 是否值得把它正式选为个人项目；
- 这个系统最终要做成什么样；
- 输入、输出、规模和鲁棒性目标是什么；
- 初版目标做多完整，哪些功能属于核心，哪些属于挑战；
- 哪些功能属于后续扩展；
- 开发环境如何准备。

后续如果需求发生变化，优先在本文档相应位置修改。对于重要方向变化，保留旧表述并使用 HTML 删除线标记，例如：

```html
<del>旧决策：第一版不支持并发写。</del>
新决策：第一版支持单写线程，后续支持多客户端并发提交到单写队列。
```

# 当前候选项目定位

如果最终决定实施，TinyLSM 暂定为：

> 一个单机、嵌入式、C++ 实现的 LSM-tree Key-Value 存储引擎，用于系统性训练存储引擎、持久化、恢复、性能与 C++ 工程能力。

<del>旧表述：一个单机、嵌入式、C++ 实现的轻量级 LSM-tree Key-Value 存储引擎。</del>

调整原因：

- 用户认为可以胜任更复杂的系统；
- 用户希望先讨论一个更完整的初版目标，再在实现中逐步靠近，而不是每做一小步都重新讨论是否扩展；
- 因此本文档把第一阶段目标从“很小的轻量练习”调整为“范围完整、分层实现、允许挑战项存在”的系统项目。

这里的“嵌入式”指 embedded database/library，即作为 C++ 库嵌入到其他程序中使用，不是指单片机或 MCU。

## 当前目标

以下均为“如果正式实施”的候选目标，不代表已经完成：

- 实现一个可链接使用的 C++ library。
- 提供一个命令行工具用于演示和调试。
- 支持基本 KV 操作、持久化、崩溃恢复、SSTable、manifest、compaction、基础并发语义和 benchmark。
- 尽早设计完整系统边界，包括文件格式、恢复语义、错误模型、测试策略和性能目标。
- 重点训练 C++ 系统开发能力：文件 IO、数据结构、持久化、恢复、性能分析和工程测试。

## 初版目标范围

初版目标不是“只做一个玩具 KV map”，而是做出一个完整但可控的存储引擎骨架。目标分三层：

### 核心必做

- `put/get/delete/scan` API。
- WAL 崩溃恢复。
- MemTable。
- SSTable 文件格式。
- Manifest 元数据。
- 多 SSTable 查询。
- Tombstone 删除语义。
- Compaction。
- CLI 工具。
- 单元测试、集成测试、恢复测试。
- 基础 benchmark。

### 增强必看，择机实现

- 简单并发写：多线程调用 API，内部串行化写入。
- `WriteBatch`：批量写入。
- Bloom Filter。
- Block Cache。
- 后台 flush / 异步 compaction。
- 文件 checksum。
- 更完整的错误模型。

### 挑战/展示项

- HTTP/TCP server：把库包装成单机 KV server。
- 极简 SQL-like shell：作为命令解析和展示功能，不做完整 SQL 数据库。
- AI benchmark 报告分析：作为辅助展示，不进入核心路径。

## 当前非目标

- 不做分布式存储。
- 不做多节点复制。
- 不做用户权限系统。
- 不做完整 SQL 数据库，但允许后期做极简 SQL-like wrapper。
- 不做 RocksDB/LevelDB API 兼容。
- 不做工业级高并发优化，但可以做基础并发 API 语义。
- 不把 AI 功能作为主线。

# 开源项目参考策略

这里的“参考”是指：阅读不同项目如何拆分问题、维护不变量、处理异常、组织测试和衡量性能，再由 TinyLSM 自己做范围裁剪和设计选择。它**不是**把现有数据库作为依赖接入，不是调用其 API 完成功能，也不是换一种语言逐行翻译源码。

TinyLSM 不以某一个仓库作为可直接套用的模板，而采用“一个主参考 + 两个互补参考 + 一个可选工程参考”的方式。

## 1. LevelDB：C++ 主参考

- 官方仓库：[google/leveldb](https://github.com/google/leveldb)
- 实现概览：[leveldb/doc/impl.md](https://github.com/google/leveldb/blob/main/doc/impl.md)
- 参考地位：**主要源码参考，优先级最高**。
- 适合参考：嵌入式有序 KV 库的边界，写入与读取路径，WAL/log record，MemTable 与 immutable MemTable，SSTable/table builder，版本与 Manifest，合并迭代器，compaction，`Status`/迭代器等接口思想，以及单元测试和 `db_bench` 的组织方式。
- 不直接照搬：公开 API、类和目录命名、二进制文件格式、默认参数、平台抽象，以及完整 compaction 实现。TinyLSM 应先写清自己的简化目标和不变量，再带着具体问题读对应源码。

LevelDB 与本项目同为 C++、单机、嵌入式、有序 KV 存储，规模和边界最接近，因此比 RocksDB 更适合做第一参考。不过它是成熟代码库，不适合从入口开始漫无目的通读。

## 2. Mini-LSM：教学路线与验收方式参考

- 官方仓库：[skyzh/mini-lsm](https://github.com/skyzh/mini-lsm)
- 配套课程：[Mini-LSM book](https://skyzh.github.io/mini-lsm/)
- 参考地位：**学习顺序和功能切片参考，不是技术栈参考**。
- 适合参考：如何把 MemTable、SST、多路合并迭代器、compaction、Manifest、WAL、恢复和并发控制拆成可运行、可测试的阶段；每一步先建立哪些不变量和测试。
- 不直接照搬：Rust 类型系统、crate 组织、异步库、接口与实现代码。TinyLSM 仍使用自己设计的 C++/CMake 工程，并独立处理资源所有权、RAII、错误返回和并发语义。

Mini-LSM 的价值恰好在于“技术栈不同”：可以迫使项目作者理解设计后再用 C++ 表达，而不是机械复制代码。但若只是把 Rust 解答逐行翻译成 C++，同样不能证明独立能力。

## 3. RocksDB：成熟设计与取舍参考

- 官方仓库：[facebook/rocksdb](https://github.com/facebook/rocksdb)
- 架构概览：[RocksDB Overview](https://github.com/facebook/rocksdb/wiki/RocksDB-Overview)
- 专题文档：[Write Ahead Log](https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-%28WAL%29)、[Compaction](https://github.com/facebook/rocksdb/wiki/Compaction)
- 参考地位：**工业级设计对照和专题资料，不作为初版模仿对象**。
- 适合参考：WAL 生命周期与恢复语义，leveled/tiered compaction 的读写放大取舍，SST block/index/filter/cache，后台任务，校验、监控指标和性能实验应关注什么。
- 不直接照搬：复杂配置系统、Column Family、事务、多种 compaction 策略、生产级缓存和线程模型。只有当 TinyLSM 已遇到相应问题时，再查一个专题并记录取舍。

RocksDB 的复杂度远高于个人教学项目。过早沿着它的代码结构实现，会让项目变成“删减版 RocksDB”，反而难以说明自己的边界和判断。

## 4. BusTub：可选的 C++ 工程组织参考

- 官方仓库：[cmu-db/bustub](https://github.com/cmu-db/bustub)
- 参考地位：**仅供参考，不是 LSM 实现参考**。
- 适合参考：CMake 工程、模块化目录、测试、格式化、sanitizer 和课程式里程碑的组织方式。
- 不适合参考：LSM 数据结构和持久化方案。BusTub 是教学型关系数据库，不应因为它是 C++ 数据库项目就把 SQL、查询执行或缓冲池加入 TinyLSM。
- 注意：遵守其仓库中的课程与学术诚信要求，不复制或公开课程作业解答。

## 分阶段阅读映射

| TinyLSM 阶段 | 先解决自己的问题 | 主要参考 |
|---|---|---|
| V0 工程骨架与内存 KV | API 所有权、错误模型、有序数据结构、测试边界 | LevelDB 的公开接口与测试组织；BusTub 只看工程组织 |
| V1 WAL 与恢复 | record 格式、截断记录、checksum、何时 `fsync`、成功返回语义 | LevelDB log/recovery；RocksDB WAL 文档 |
| V2 SSTable 与 Manifest | 文件布局、索引、原子发布、重启时识别有效文件 | LevelDB implementation/table/version；Mini-LSM 对应阶段 |
| V3-V5 多表读取、删除、compaction、scan | 新旧版本优先级、tombstone、多路归并、compaction 前后等价 | LevelDB 对应源码；Mini-LSM 的阶段与测试；RocksDB compaction 文档 |
| V6-V8 并发、可靠性与性能 | 锁与可见性、后台任务生命周期、故障注入、指标设计 | 先用自己的测试暴露问题，再按专题查 LevelDB/RocksDB；benchmark 方法与自身基线对比 |

## 怎样证明这是自己的项目

对每个重要机制，保留一份简短设计记录：

1. TinyLSM 要解决的具体问题和不变量；
2. 查阅了哪些资料，各自采用什么思路；
3. 最终选择、删减或改动了什么，理由是什么；
4. 用什么测试、故障注入或 benchmark 验证；
5. 当前限制和下一步是什么。

项目价值不来自宣称“完全原创”，而来自能够解释：为什么这样设计、和参考实现有什么差异、哪些结果已经被测试或测量。若最终实施，应在 README 中公开列出参考资料；若复制了受许可证约束的代码，则必须另行遵守许可证和署名要求。

# 数据模型

## 基本模型

TinyLSM 第一阶段采用 Key-Value 模型：

```text
key -> value
```

暂定：

- key：byte string，第一版用 `std::string` 表示；
- value：byte string，第一版用 `std::string` 表示；
- key 按字典序排序；
- value 不解释业务含义。

## 基本操作

计划支持：

```cpp
Status put(std::string_view key, std::string_view value);
Result<std::string> get(std::string_view key);
Status del(std::string_view key);
Iterator scan(std::string_view start_key, std::string_view end_key);
```

语义：

- `put(k, v)`：写入或覆盖 key。
- `get(k)`：返回最新 value；不存在则返回 NotFound。
- `del(k)`：写入删除标记 tombstone，不立即物理删除旧数据。
- `scan(a, b)`：返回 `[a, b)` 范围内按 key 有序排列的未删除记录。

# SQL 相关讨论

用户提出：SQL 是否是常用技术，能否加入本项目。

当前判断：

- SQL 确实是数据库领域非常常用的技术，但它通常属于查询层 / 执行层。
- TinyLSM 当前核心训练目标是存储层：WAL、MemTable、SSTable、compaction、恢复。
- 如果过早加入 SQL，项目会膨胀成 MiniDB，范围会明显扩大。

当前暂定：

- 第一阶段不做 SQL。
- 后续可以考虑做一个极简 SQL-like wrapper，但只作为上层扩展。

可能的后续扩展：

```sql
PUT user:1 tina;
GET user:1;
DELETE user:1;
SCAN user:1 user:9;
```

或者更接近 SQL 的极小子集：

```sql
CREATE TABLE kv (key TEXT PRIMARY KEY, value TEXT);
INSERT INTO kv VALUES ('user:1', 'tina');
SELECT value FROM kv WHERE key = 'user:1';
```

待确认：

- 是否需要把 SQL 作为后期展示功能；
- 如果做 SQL，是做命令解析器，还是做真正的表/查询执行层。

# 系统规模

当前规模目标：

> 支持百万级 key-value 数据的单机轻量存储引擎，数据库大小在数百 MB 到数 GB 级别，重点验证正确性、恢复能力和性能趋势。

阶段性规模：

| 阶段 | 数据量 | 目标 |
|---|---:|---|
| V0-V1 | 1K - 10K records | 验证 API、MemTable、WAL 与恢复 |
| V2-V4 | 100K - 1M records | 验证 SSTable、多文件查询与 compaction |
| V5+ | 1M - 5M records | benchmark、性能分析、磁盘占用分析 |

典型数据形态：

- key：16B / 32B / 64B；
- value：100B / 1KB / 4KB；
- 数据量：100K / 1M / 5M records。

# 核心架构

初步模块：

| 模块 | 职责 |
|---|---|
| `DB` | 对外 API，协调读写路径 |
| `MemTable` | 内存中的有序表，接收最新写入 |
| `WAL` | Write-Ahead Log，保证崩溃恢复 |
| `SSTable` | 磁盘上的有序不可变数据文件 |
| `Manifest` | 记录当前有效 SSTable 文件集合和版本信息 |
| `Compactor` | 合并 SSTable，清理旧值和 tombstone |
| `Iterator` | 支持 scan/range query |
| `Benchmark` | 写入、读取、扫描、恢复和 compaction 性能测试 |

写路径：

```text
put/delete
  -> append WAL
  -> update MemTable
  -> MemTable 达到阈值后 flush 成 SSTable
```

读路径：

```text
get
  -> 查 MemTable
  -> 查 immutable MemTable（如有）
  -> 从新到旧查 SSTable
  -> 遇到最新值或 tombstone 后返回
```

# 鲁棒性需求

第一阶段重点：

- `put/delete` 成功返回后，崩溃重启应能通过 WAL 恢复。
- 重启后可以恢复 MemTable 中尚未 flush 的数据。
- 多个 SSTable 有相同 key 时，读取最新值。
- delete 后不能读出旧值。
- scan 结果有序，且不返回已删除 key。

后续增强：

- WAL record checksum。
- SSTable block checksum。
- flush 使用临时文件 + rename，避免半成品文件污染数据库。
- Manifest 更新具备原子性。
- 文件损坏时返回明确错误。
- 磁盘空间不足、权限错误等异常有明确错误路径。

# 并发策略

用户提出：为什么不做复杂并发写，是否可以做简单并发写。

当前判断：

- 复杂并发写会引入锁粒度、写入顺序、WAL group commit、读写一致性、compaction 并发可见性等问题。
- 这些问题有价值，但不适合第一版和存储格式、恢复逻辑同时展开。

当前暂定：

- 第一阶段采用单 writer 模型。
- 可以允许多个线程调用 API，但内部用全局 mutex 串行化写操作。
- 读操作第一版也可以简单加锁保证正确性。

后续可能演进：

- 多读单写锁：`std::shared_mutex`。
- 后台 flush / compaction 线程。
- 写请求进入队列，由单写线程顺序处理。
- WriteBatch 合并多个写入，减少 WAL flush 次数。

# 版本路线

本项目采用“完整目标先定，分阶段逼近”的路线。每个版本都是向完整系统靠近，不代表每个版本结束都要重新讨论是否扩展。

| 版本 | 内容 | 验收标准 |
|---|---|---|
| V0 | 工程骨架 + 纯内存 KV | CMake、library、CLI、tests 跑通；`put/get/delete/scan` 语义正确 |
| V1 | WAL + 恢复 | `put/delete` 写 WAL；重启后可恢复；有基础 crash/reopen 测试 |
| V2 | SSTable + Manifest | MemTable 满后刷盘；SSTable 文件可读取；Manifest 记录有效文件 |
| V3 | 多 SSTable + Tombstone | 新值覆盖旧值；delete 不读出旧值；多文件查询顺序正确 |
| V4 | Compaction | 合并 SSTable；清理旧版本和 tombstone；compaction 前后结果一致 |
| V5 | Iterator/Scan 完整化 | MemTable + 多 SSTable 合并迭代；范围查询有序且正确 |
| V6 | 基础并发 + WriteBatch | 多线程调用 API 不破坏正确性；批量写入可用 |
| V7 | 性能与可靠性 | benchmark、恢复测试、flush/manifest 异常测试、性能报告 |
| V8 | 读性能优化 | Bloom Filter、Block Cache 至少实现其中一个，并有对比实验 |
| V9 | 服务化/展示扩展 | 可选 HTTP/TCP server 或 SQL-like shell，不影响核心 library |

# 可选扩展解释

## Bloom Filter

用于快速判断某个 key 是否大概率不在某个 SSTable 中。

价值：

- 减少无意义磁盘读取；
- 提升读取不存在 key 时的性能。

状态：后期优化，不是第一版必需。

## Block Cache

缓存 SSTable 中已经读过的数据块。

价值：

- 热点 key 重复读取时减少磁盘 IO；
- 训练缓存设计和淘汰策略。

状态：后期优化。

## HTTP/TCP server

给 TinyLSM 加一层服务外壳，让用户可以通过网络请求访问：

```http
PUT /kv/user:1
GET /kv/user:1
DELETE /kv/user:1
```

价值：

- 轻量补网络编程；
- 让 TinyLSM 从 library 变成单机 KV server。

状态：后期可选，不作为主线。

## 异步 compaction

把 SSTable 合并放到后台线程做，前台读写不必同步等待 compaction 完成。

价值：

- 更接近真实 LSM 系统；
- 训练后台线程、版本可见性和资源生命周期管理。

状态：中后期功能。

## 批量写 WriteBatch

一次提交多个 put/delete：

```cpp
WriteBatch batch;
batch.put("a", "1");
batch.del("b");
db.write(batch);
```

价值：

- 减少 WAL 写入/flush 次数；
- 提升批量导入性能；
- 为后续 group commit 铺路。

状态：建议中期加入。

## AI 分析 benchmark 报告

把 benchmark 输出交给 LLM，让它总结性能变化和可能瓶颈。

价值：

- 作为 AI 辅助能力；
- 不影响 C++ 主线。

状态：可选展示功能。

# 开发环境

用户当前环境：

- 主力电脑：macOS；
- 另有一台带 Linux 虚拟机的电脑。

当前判断：

- macOS 可以作为日常 C++ 开发环境；
- TinyLSM 涉及文件系统、fsync、rename、mmap 等行为，最终必须在 Linux 环境验证；
- 推荐使用 Mac 日常开发 + Docker/Linux 验证；
- 如果 Linux 虚拟机电脑使用体验稳定，也可以作为主要验证环境。

Mac 侧建议工具：

```bash
xcode-select --install
brew install cmake ninja llvm googletest google-benchmark spdlog fmt
```

Linux 侧建议工具：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build clang gdb git \
  libgtest-dev libbenchmark-dev
```

待确认：

- 是否使用 Docker；
- 是否使用 Linux 虚拟机作为主开发环境；
- 是否配置 GitHub Actions 做 Linux CI。

# 压测与硬件条件

TinyLSM 不是面向单片机的系统，而是面向普通操作系统进程的 embedded KV storage library。

当前硬件可以做压测，但压测目标不是“证明它打败工业数据库”，而是观察：

- 数据量上升后吞吐如何变化；
- value 大小变化后写入和读取如何变化；
- compaction 前后读性能如何变化；
- Bloom Filter / Block Cache 是否改善读取性能；
- WAL fsync 策略对写入延迟有什么影响。

后续可考虑有限资源测试：

- 限制内存；
- 限制数据库目录大小；
- 使用较小 MemTable；
- 人为增加大量 SSTable；
- 模拟低速磁盘或频繁 fsync。

# 待确认问题

- 完成基础概念学习和参考项目定向阅读后，是否正式选择 TinyLSM 作为求职项目？
- 是否完全不做 SQL，还是后期做极简 SQL-like wrapper？
- 第一版并发语义如何写进 API 文档？
- `put` 成功返回是否要求 WAL 已 fsync？
- WAL 和 SSTable 的初始二进制格式如何设计？
- Manifest 更新是否从第一版就做原子 rename？
- 是否使用 Docker 作为标准 Linux 开发/测试环境？
- 项目名是否确定为 TinyLSM？

# 对项目文档的影响

后续应拆分出：

- `docs/design.md`：架构、读写路径、文件格式、错误模型；
- `docs/roadmap.md`：阶段计划和验收标准；
- `docs/test-plan.md`：单测、恢复测试、benchmark；
- `docs/decisions/`：SQL、并发、fsync、文件格式等重要决策。
