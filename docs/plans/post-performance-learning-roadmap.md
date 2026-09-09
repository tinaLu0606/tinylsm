# TinyLSM 性能阶段之后的学习路线建议

## 文档状态

- 日期：2026-09-08
- 性质：调研与建议，不代表已经实现，也不自动授权执行
- 当前基础：读取缓存与并发读、bounded immutable MemTable/background flush、
  partial/background compaction 已完成
- 核心目标：继续增加系统软件深度，而不是把 TinyLSM 无限制扩成 RocksDB

## 1. 结论先行

最推荐的下一条主线是：

```text
Public Iterator
      ↓
Snapshot（一致性快照）
      ↓
MVCC（同一个 key 保留多个版本）
      ↓
Watermark + compaction GC（安全回收旧版本）
```

这是 TinyLSM 当前最自然的下一步。现有引擎已经有 sequence number、并发读取、
后台 flush、后台 compaction 和 tombstone；MVCC 会迫使这些模块真正围绕“某个读者
能看见哪个版本”协同工作，学习价值高于继续增加另一个孤立的缓存或调参项。

第二优先级是 SSTable v2：prefix compression、restart point、properties/index
metadata，并且只在新的 negative-lookup 数据支持时加入 Bloom Filter。第三优先级是
由 multiwriter sync benchmark 驱动的 Group Commit。除此之外，建议并行补充
model-based/crash testing，增强项目的可信度。

## 2. 当前 TinyLSM 已经走到哪里

当前数据生命周期是：

```text
Put/Delete/WriteBatch
        ↓
active WAL + active MemTable
        ↓  MemTable 写满
immutable WAL + immutable MemTable
        ↓  background flush
SSTable
        ↓  table pressure 达到阈值
background partial compaction
```

读取会合并 active/immutable MemTable 和多个 SSTable；Block Cache 避免反复读取、
校验和解码热点 block。当前的 sequence number 主要用于判断“哪个写入更新”，但公共
API 只能读最新值，`Scan` 也会一次性生成完整 `vector<Entry>`。

因此现在缺少的最重要能力不是“把单次 Get 再加速一点”，而是：

> 当写入和 compaction 继续发生时，一个读者如何稳定地看见数据库过去某一时刻的
> 完整状态？旧版本又在什么时候才能安全删除？

这正是 Snapshot 和 MVCC 解决的问题。

## 3. 类似项目给出的启发

### LevelDB：小而完整的公共存储接口

LevelDB 除了 Put/Get/Delete，还提供 Iterator、Snapshot、WriteBatch、Bloom Filter、
可替换 Comparator 和 Env/FileSystem 抽象。Snapshot 表示一个一致的只读视图；使用者
释放 Snapshot 后，引擎才可以回收为它保留的状态。LevelDB 也明确指出 Bloom Filter
适合工作集放不进内存、并且随机读取较多的 workload。

参考：

- [LevelDB documentation](https://github.com/google/leveldb/blob/main/doc/index.md)
- [LevelDB implementation notes](https://github.com/google/leveldb/blob/main/doc/impl.md)
- [LevelDB table format](https://github.com/google/leveldb/blob/main/doc/table_format.md)

对 TinyLSM 的启发：下一步应扩展“读取语义”，而不只是扩展“性能选项”。

### Mini-LSM：完成存储与 compaction 后进入 MVCC

Mini-LSM 的学习顺序与 TinyLSM 很接近：先完成 MemTable、SST、读写、compaction、
Manifest 和 WAL，然后进入 timestamp key、snapshot read、watermark、garbage
collection、optimistic concurrency control 和 serializable validation。

参考：

- [Mini-LSM repository and course structure](https://github.com/skyzh/mini-lsm)
- [Mini-LSM MVCC overview](https://skyzh.github.io/mini-lsm/week3-overview.html)
- [Mini-LSM SST optimizations](https://skyzh.github.io/mini-lsm/week1-07-sst-optimizations.html)

对 TinyLSM 的启发：MVCC 是把 sequence、iterator、compaction 和并发控制连接起来的
综合主题，适合作为下一阶段主线。

### RocksDB：学习设计边界，不复制功能数量

RocksDB 的 Snapshot 与内部 sequence number 关联：一般来说，版本 sequence 不大于
Snapshot sequence 才对该读者可见。它也区分 Iterator 和 Snapshot 的生命周期，并
提供 prefix seek、transactions、rate limiting 等大量面向真实 workload 的机制。

参考：

- [RocksDB Snapshot](https://github.com/facebook/rocksdb/wiki/Snapshot)
- [RocksDB overview](https://github.com/facebook/rocksdb/wiki/RocksDB-Overview)
- [RocksDB Prefix Seek](https://github.com/facebook/rocksdb/wiki/Prefix-Seek)
- [RocksDB Transactions](https://github.com/facebook/rocksdb/wiki/Transactions)

对 TinyLSM 的启发：可以学习“sequence 决定可见性”和“资源由谁持有”，但不应直接
复制 column family、多层 scheduler、事务数据库等成熟系统的完整复杂度。

## 4. 建议一：Public Iterator + Snapshot + MVCC（最高优先级）

### 4.1 名词先解释

**Iterator（迭代器）**：不是一次把所有 Scan 结果装进一个 vector，而是每次只取
当前一条，再调用 `Next()` 取下一条。

```text
当前 Scan：读取完整范围 -> 生成 vector -> 返回

Iterator：Seek("cat") -> 当前 cat
                     -> Next() 得到 dog
                     -> Next() 得到 fox
```

好处是大范围扫描不需要一次复制全部结果，也为 `Seek/Next`、长时间扫描和 Snapshot
建立合适的 API。

**Snapshot（快照）**：记录数据库某个时刻的逻辑版本。例如在 sequence=100 创建
快照，之后即使 `name` 在 sequence=101 被修改，快照仍应读到 sequence<=100 的值。

**MVCC（Multi-Version Concurrency Control，多版本并发控制）**：同一个 key 暂时
保留多个历史版本，让旧快照和新读者同时得到各自正确的值。

```text
name@105 = Tina-new
name@100 = Tina-old

Snapshot(102) -> Tina-old
最新读取       -> Tina-new
```

**Watermark（水位线）**：所有活跃快照中最老的 sequence。比它更旧、并且不可能再被
任何读者需要的版本，才可以在 compaction 中回收。

### 4.2 为什么适合 TinyLSM

TinyLSM 已经给每次写入分配 sequence，SSTable 也保存 sequence；已有 background
compaction 和 shared reader ownership。现在只保存/返回最新逻辑值，正好可以沿已有
结构继续深入，而不是换一个项目重新学习。

它会训练四个很重要的系统能力：

1. **可见性规则**：不是“文件里有什么就返回什么”，而是选择 `sequence <= snapshot`
   的最新版本。
2. **生命周期管理**：Snapshot、Iterator、MemTable 和 SSTable Reader 谁拥有谁，何时
   可以释放。
3. **并发正确性**：写入、flush、compaction 继续发生时，旧快照仍保持一致。
4. **垃圾回收**：compaction 不能因为看到更新版本，就删掉仍被旧快照需要的版本或
   tombstone。

### 4.3 建议拆成三个 checkpoint

```text
A. Public Iterator
   Seek / Valid / key / value / Next / status
   明确 iterator 持有哪些 reader 和内存状态

B. Snapshot visibility
   internal key 改为 user_key + sequence
   Get/Iterator 接受 snapshot sequence
   同一个 key 保留多版本

C. Watermark + compaction GC
   跟踪最老活跃 snapshot
   只回收已证明不可见的旧版本/tombstone
```

### 4.4 验收重点

- 创建 Snapshot 后反复覆盖、删除同一个 key，旧快照结果不变。
- Snapshot 跨越 MemTable rotation、background flush 和 compaction 后仍正确。
- 释放最老 Snapshot 后，compaction 可以回收不再需要的版本。
- Iterator 长时间存在时，旧 SSTable 不产生悬空引用；Close 行为明确。
- 崩溃恢复后只恢复持久化状态；内存 Snapshot 不跨进程重启。
- TSan、ASan/UBSan、故障注入和版本数/空间占用 benchmark 通过。

### 4.5 控制范围

第一阶段只做只读 Snapshot，不立即做完整事务。Snapshot 回答的是“我能看见哪个
版本”；事务还要回答“多个读写是否冲突、是否全部提交”，是下一层问题。

## 5. 建议二：SSTable v2 格式实验（第二优先级）

当前 SSTable 已有 block、index 和 checksum，但仍可以成为一个很好的二进制格式
实验场。建议把它作为独立 Goal，保持旧格式可读，并做真实空间/CPU/I/O 对比。

### 5.1 Prefix compression

**前缀压缩**利用已排序 key 往往拥有共同开头的特点：

```text
原始：user:000001
      user:000002
      user:000003

压缩：user:000001
      [共享 10 字节] + "2"
      [共享 10 字节] + "3"
```

它能减小 SSTable，但读取时需要重建 key，会增加 CPU 成本。

### 5.2 Restart point

如果每个 key 都依赖前一个 key，随机查找必须从 block 开头一路解码。**Restart
point（重启点）**就是每隔 N 条保存一个完整 key，使查找可以先跳到附近再顺序解码。

```text
完整 key -> 差量 -> 差量 -> 差量 -> 完整 key -> 差量 ...
   ^ restart                         ^ restart
```

它是在“压缩率”和“随机查找成本”之间做可测量折中。

### 5.3 Properties/index metadata

当前 Open 会扫描所有 live SSTable data blocks 来验证 sequence metadata，成本是
`O(total live SSTable bytes)`。可以设计带 checksum 的 properties block，保存 entry
count、key range、sequence range 等信息；但必须先定义它能证明什么、不能证明什么，
不能为了启动快而悄悄弱化损坏检测。

### 5.4 Bloom Filter 仍然是条件项

Bloom Filter 可以回答：

```text
“这个 key 一定不在表里” -> 可以跳过 SSTable
“这个 key 可能在表里”   -> 仍要正常查找
```

它可能误报“可能存在”，但不能把真实存在的 key 判断成不存在。当前 Goal 1 数据没有
证明 cache 后的 negative lookup 仍是主要问题，所以先建立大于 cache 的冷数据集和
高 miss-ratio workload；只有 table/block I/O 仍显著时才实现。

## 6. 建议三：证据驱动的 Group Commit（条件优先级）

**Group Commit（组提交）**是把多个并发 writer 的 WAL 写入合并，让它们共享一次
`fsync`：

```text
writer A ─┐
writer B ─┼-> writer queue -> 一个 WAL batch -> 一次 fsync -> 分别返回结果
writer C ─┘
```

这不是让多个线程无序修改 MemTable。通常会选一个 leader 分配连续 sequence、写 WAL
并同步，其他 writer 等待各自结果；这样既减少 fsync 次数，也保持确定的提交顺序。

它很适合练习 C++ 并发：mutex、condition_variable、队列、leader/follower、唤醒与
错误广播。但只有下面的 benchmark/profile 成立时才建议执行：

- 4/8/16 个 writer，固定总操作数与 key/value 大小；
- `sync_on_write=true`；
- WAL fsync 或 writer lock wait 是主要成本；
- 报告吞吐、p50/p95/p99、每次 fsync 合并的 writer 数量和失败传播。

如果 workload 主要是单 writer 或 `sync_on_write=false`，Group Commit 的学习价值仍在，
但不能声称它会明显改善该 workload。

## 7. 建议四：Model-based testing + 系统化 crash testing

这是可以和任何主线并行推进的工程能力。

**Model-based testing（基于模型的测试）**：用一个简单、明显正确的 `std::map` 作为
参考模型，随机生成 Put/Delete/WriteBatch/Get/Scan，再比较 TinyLSM 的结果。

```text
同一串随机操作
   ├-> std::map 参考结果
   └-> TinyLSM 实际结果
             ↓
          必须一致
```

**Crash testing（崩溃测试）**：不只是让某个文件 API 返回错误，而是在 WAL、Manifest
rename、directory sync、SSTable publication 等时刻强制终止进程，再重新 Open，检查
已确认写入、未确认写入和文件清理是否符合合同。

推荐加入：

- 固定随机种子，失败可复现；
- 保存最小操作 trace，失败后可以缩减；
- 在每个 durable boundary 前后 crash；
- 长时间并发读写/flush/compaction 后 reopen；
- 对 WAL、Manifest、SSTable decoder 做 fuzzing（自动生成畸形字节输入）。

这类工作不一定产生漂亮的 ops/s，但非常能证明你理解持久化系统的正确性。

## 8. 暂不建议优先做什么

### 完整 RocksDB 式 leveled compaction

它会引入 level metadata、overlap selection、compaction score、并行调度、限速和大量
参数。当前 simplified size-tiered 已能展示读放大、写放大和 table pressure 的权衡；
除非更大数据集证明它成为主要边界，否则容易变成“代码很多，但学习主线不清楚”。

### 完整事务系统

事务还需要冲突检测、隔离级别、提交/回滚和死锁/重试策略。先把 Snapshot、MVCC 和
watermark 做正确，再考虑 optimistic transaction；否则会同时改变太多不变量。

### 分布式复制或 Raft

Raft 是让多台机器对日志顺序达成一致的共识算法。它本身就是一个完整项目，会把注意力
从本地存储引擎转向网络、成员变化和分布式故障。TinyLSM 尚未形成稳定 Snapshot/
Checkpoint API 前，不建议把复制作为主线。

### 继续扩展 UI

Lab 已能观察内部状态。除非某个新机制需要新的可视化，例如 MVCC 版本链或 watermark，
否则继续增加 UI 页面对 C++ 存储内核能力的提升有限。

## 9. 推荐执行顺序

```text
Goal 4（推荐主线）
Public Iterator + Snapshot + MVCC + Watermark GC
        ↓
Goal 5（二进制格式实验）
SSTable v2：prefix compression + restart points + properties
        ↓
按数据二选一
        ├-> 高并发 sync 写慢：Group Commit
        └-> 冷数据 negative miss 慢：Bloom Filter

贯穿全部 Goal：model-based / crash / fuzz testing
```

如果只选择一个下一步，选择 Goal 4。它最能把 TinyLSM 从“功能完整的 LSM demo”推进到
“能够解释版本可见性、资源生命周期和并发语义的学习型存储引擎”。

## 10. 每个新 Goal 的共同执行合同

- 先定义一个具体 workload 或正确性场景，再决定实现。
- 写清状态所有权、锁顺序、持久化 commit point 和失败后的可见状态。
- 保留旧格式读取兼容；格式变化必须有 golden file 和 corruption tests。
- 使用小例子证明语义，再做 benchmark；不能只用吞吐代表正确。
- 至少完成 Debug、Release、ASan/UBSan、TSan、fault/reopen 和固定 Linux 证据。
- 报告收益，同时报告内存、空间放大、写放大、延迟或实现复杂度代价。
- 一个 Goal 一条核心学习主线，避免同时加入多个互不依赖的成熟数据库功能。

上述建议的工程拆分、正确性/故障矩阵和专项 before/after 验收，见
`docs/plans/next-engineering-goals.md`。
