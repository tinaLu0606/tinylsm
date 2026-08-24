# 2026-08-24 · TinyLSM · V0-V2 架构与持久化基础学习笔记

## 记录来源

- 类型：相关知识学习
- 来源：用户阅读初版架构后的复述、问题，以及 AI 按逻辑主线重新整理的讲解
- 状态：`仅供参考`；这是设计学习材料，不代表代码已经实现或验证
- 相关计划：[`../plans/v0-v2-source-design.md`](../plans/v0-v2-source-design.md)、[`../plans/v0-v2-repository-design.md`](../plans/v0-v2-repository-design.md)
- Notion 索引：[项目文档与记录 · TinyLSM V0-V2 架构与持久化基础学习笔记](https://app.notion.com/p/3c6040fbdca48145a11cee242d55e145)
- 同步关系：本地 Markdown 是正文与技术细节的唯一权威源；Notion 只维护摘要、项目归属和索引，本地路径或核心结论变化时同步更新

## 原始理解与需要校正的地方

用户的核心理解是：

> 写入数据时先记录 WAL，再更新 MemTable；MemTable 满后写成 SSTable。WAL 可以在崩溃后恢复数据。

这条主线正确，需要补充三个边界：

1. “追加 WAL”不必然等于“数据已进入物理持久介质”。`write()` 通常先进入操作系统 page cache；是否要求立即持久化由 `sync_on_write` 和 `fsync` 语义决定。
2. WAL、MemTable、SSTable 不是三份功能重复的数据：WAL 负责恢复，MemTable 负责快速更新和排序，SSTable 负责长期、不可变、有序的磁盘存储。
3. V2 重启时不会把所有 SSTable 重新装进 MemTable。数据库通过 Manifest 打开有效 SSTable，只重放当前 WAL，重建尚未 flush 的 MemTable。

## 一条主线：一次写入如何变成可恢复的磁盘状态

TinyLSM 要同时解决两个矛盾：

- 用户希望 `Put` 很快；
- 数据库又不能因为进程或系统崩溃随意丢失已经确认的写入。

LSM-tree 的基本办法是把一次写入拆成两个方向：

```text
Put(key, value)
    |
    +--> WAL：按到达顺序追加，提供恢复证据
    |
    +--> MemTable：在内存中更新并按 key 维护有序状态
                         |
                         | 收集到一定大小
                         v
                    SSTable：批量、有序、不可变地写入磁盘
```

WAL 使“小写入”可以用顺序追加完成；MemTable 把许多小写入聚合并排序；SSTable 把聚合结果一次性顺序写入磁盘。这样避免每次 `Put` 都修改复杂的磁盘索引或重写一个有序文件。

## 四种核心状态各自解决什么问题

| 组件 | 主要位置 | 组织方式 | 生命周期 | 主要职责 |
|---|---|---|---|---|
| WAL | 磁盘文件 | 按操作到达顺序追加 | 临时；对应数据进入 SSTable 后可删除 | 崩溃恢复、确认写入顺序 |
| MemTable | 内存 | 按 key 有序，可修改 | 临时；flush 后释放 | 快速读写、聚合和排序 |
| SSTable | 磁盘文件 | 按 key 有序，创建后不可修改 | 长期；直到以后被 compaction 替换 | 持久保存、点查和范围读取 |
| Manifest | 磁盘元数据 | 当前数据库文件集合和编号状态 | 长期、受控更新 | 声明哪些 SSTable/WAL 属于当前有效版本 |

### 一个具体例子

用户依次执行：

```text
seq=100 Put("cat", "7")
seq=101 Put("apple", "2")
seq=102 Put("cat", "8")
```

WAL 保留操作顺序：

```text
100 Put cat   7
101 Put apple 2
102 Put cat   8
```

MemTable 保留当前可见状态，并按 key 排序：

```text
apple -> 2
cat   -> 8
```

如果此时崩溃，数据库按 sequence 顺序重放 WAL，可以重新得到相同的 MemTable。

如果此时 flush，SSTable 可能包含：

```text
apple -> 2
cat   -> 8
```

Manifest 随后发布这个 SSTable，并指定新的活跃 WAL。只有发布成功后，旧 WAL 才能安全清理。

## 为什么不能每次 Put 都直接写 SSTable

SSTable 的两个关键特征是：

- 内容按 key 排序；
- 创建完成后不可原地修改。

假设现有 SSTable 是：

```text
a -> 1
c -> 3
```

现在写入 `b -> 2`。如果直接修改 SSTable，要么重写为：

```text
a -> 1
b -> 2
c -> 3
```

要么为每次写入创建一个极小 SSTable。前者造成严重写放大，后者造成大量小文件和昂贵查询。每次还要重建 index/footer、计算 checksum、同步文件并更新 Manifest。

因此采用：

```text
单次操作 -> WAL 顺序追加
多次操作 -> MemTable 聚合和排序
一批操作 -> SSTable 批量顺序写入
```

这就是 LSM-tree 将许多细碎随机更新转换为顺序写和批量写的基本思想。

## 写、读、恢复和 flush 的完整流程

### 写入

```text
DB::Put/Delete
    -> 分配 sequence number
    -> 把逻辑操作编码为 WAL record
    -> 追加 WAL
    -> 按 durability 配置决定是否 fsync
    -> 更新 MemTable
    -> 返回 Status
```

核心不变量：如果 WAL 阶段失败，不能继续修改 MemTable，否则会出现“当前进程读得到、重启后却消失”的假成功。

### 点查

V2 的简化读路径：

```text
DB::Get(key)
    -> 先查 MemTable
       -> 找到 Value：直接返回
       -> 找到 Tombstone：返回 NotFound
       -> 没找到：继续
    -> 查 Manifest/VersionSet 指定的 SSTable
    -> 返回 Value 或 NotFound
```

先查 MemTable 是因为它保存比已落盘 SSTable 更新的状态。

### 重启恢复

```text
DB::Open(path)
    -> 读取 Manifest
    -> 打开 Manifest 声明为有效的 SSTable
    -> 打开 Manifest 声明的活跃 WAL
    -> 顺序重放合法 WAL record
    -> 重建尚未 flush 的 MemTable
    -> 恢复 last_sequence
```

V1 没有 SSTable，因此可能重放整个 WAL；V2 已有 SSTable 后，只需要重放当前尚未 flush 的 WAL。

### MemTable flush

这里的 flush 指“把 MemTable 转换并发布为 SSTable”，不是简单的 C++ stream flush：

```text
停止修改待 flush 的 MemTable
    -> 按 key 顺序写 <number>.sst.tmp
    -> 校验并同步临时 SSTable
    -> rename 为正式 SSTable
    -> 原子更新 Manifest
    -> Manifest 持久化后清理旧 WAL
    -> 释放已 flush 的 MemTable
```

V2 暂定只验收一个已发布 SSTable。连续多次 flush、多 SSTable 新旧版本选择和完整 tombstone 语义属于 V3。

## Sequence number：数据库内部的逻辑时间

Sequence number 是每次逻辑写操作的单调递增编号，不是时间戳、文件偏移或 SSTable 编号。

```text
seq=10 Put("cat", "7")
seq=11 Put("cat", "8")
seq=12 Delete("cat")
```

`seq=12` 最新，因此 `cat` 当前表现为 NotFound。Sequence number 用于：

- 判断同一 key 哪个版本更新；
- 保持 WAL 重放顺序；
- 让 tombstone 覆盖旧 value；
- 在 Manifest 中保存 `last_sequence`；
- 为未来 snapshot/一致性读取留下基础。

Sequence 可以有空洞，但不能重复或倒退。

## 编码：把逻辑记录变成稳定字节

C++ 对象不能直接把 `sizeof(object)` 个字节写进 WAL，因为 `std::string` 内部含指针，结构体还有 padding、字节序和 ABI 问题。

一个简单 WAL record 可表示为：

```text
record_length | checksum | sequence | type | key_length | value_length | key | value
```

编码负责：

- 把整数按明确字节序写入字节数组；
- 把变长字符串写成 `length + bytes`；
- 计算 checksum；
- 写入格式版本。

解码负责反向恢复，并且必须先验证长度上限和 checksum，再分配内存或信任内容。

### Protobuf 能否代替编码

可以用 Protobuf 表达逻辑 payload，例如：

```proto
message WalRecord {
  uint64 sequence = 1;
  enum Type { PUT = 0; DELETE = 1; }
  Type type = 2;
  bytes key = 3;
  bytes value = 4;
}
```

但 Protobuf 只解决对象到字节的序列化，不能自动解决：

- record 从哪里开始、到哪里结束；
- 文件尾部只有半条记录时如何识别；
- checksum 和中部损坏检测；
- `fsync` 和成功返回语义；
- SSTable 的 block/index/footer 布局；
- Manifest 的原子发布。

即使用 Protobuf，WAL 仍需要外层 framing：

```text
length | checksum | protobuf_payload
```

当前建议是 V1/V2 核心格式采用自己实现的最小二进制编码，以真正学习 framing、校验、恢复和磁盘格式；Protobuf 可以在 V2 完成后作为替代 Manifest 编码的对比实验。该建议仍待用户确认。

## `write`、用户态 flush、`fsync` 的区别

数据路径可以简化为：

```text
应用缓冲
   -> write/stream flush
操作系统 page cache
   -> fsync
持久化设备
```

- C++ stream 的 `flush()`：主要把用户态缓冲交给内核，不等于物理持久化。
- `write()` 成功：通常表示内核接受了数据，不等于系统断电后一定存在。
- `fsync(fd)`：要求操作系统把该文件的脏数据和必要元数据推进到持久化边界。
- 文件创建/rename 后要保证目录项持久化，还需要考虑数据库目录的同步。

`sync_on_write=true` 表示每次 Put/Delete 在 WAL 同步成功后才向用户返回成功；可靠性强但延迟较高。`false` 可以批量同步、吞吐更高，但最近确认的写入可能在系统崩溃或断电后丢失，因此必须把语义写清楚。

V1 前需要补充的 OS 关键词：file descriptor、page cache、short write、`open/read/write/close`、`fsync/fdatasync`、错误码。V2 前再学习 atomic rename、directory fsync、文件截断与 crash consistency。

## Manifest 与 VersionSet

Manifest 不是 `key -> 硬盘块` 页表，而是数据库有效文件集合的持久化总账：

```text
format_version
active_wal_number
next_file_number
last_sequence
live_sstables[]
```

每个 SSTable 元数据可以包含：

```text
file_number
file_size
smallest_key
largest_key
min_sequence
max_sequence
```

Manifest 决定哪个 SSTable 已正式成为数据库的一部分。目录中存在但未被 Manifest 引用的 `.sst` 或 `.tmp` 文件不能自动当作有效数据。

VersionSet 通常是从 Manifest 恢复出的内存对象：

```cpp
struct Version {
  uint64_t active_wal_number;
  uint64_t next_file_number;
  uint64_t last_sequence;
  std::vector<TableMeta> live_tables;
};

class VersionSet {
 public:
  Status Recover(const Path& manifest);
  Status LogAndApply(const VersionEdit& edit);
  std::shared_ptr<const Version> Current() const;
};
```

页表用于细粒度地址翻译；Manifest 只声明有效文件及其元数据。真正把 key 范围定位到 SSTable 内部 data block 的是 SSTable index。

在 V2 中只有一个当前版本，可以先实现简化的 `ManifestState`；到 V3/V4 出现多个 SSTable、compaction 和并发读版本后，再扩展为完整 VersionSet。

## C++ 工程边界

### `include/tinylsm/`

CMake 将 `include/` 加入搜索路径后，调用者可以写：

```cpp
#include <tinylsm/db.h>
```

项目名子目录用于避免 `db.h`、`status.h` 等通用名称与其他库冲突，也形成自然安装布局：

```text
/usr/local/include/tinylsm/db.h
/usr/local/lib/libtinylsm.a
```

### `tools/`

`tools/` 放使用 library 的可执行程序，而不是核心存储逻辑：

- V0：`tinylsm_cli`；
- V1：可选 `wal_dump`；
- V2：可选 `sstable_dump`、`manifest_dump`。

### `src/`

- `db/`：公开 API 的实现和读写/恢复协调；
- `memtable/`：有序内存状态；
- `wal/`：WAL record 编解码、追加和重放；
- `io/`：文件读写、同步、rename、错误转换；
- `sstable/`：有序不可变文件的 builder/reader；
- `manifest/`：有效文件集合持久化和内存版本视图；
- `util/`：字节编码、checksum 等底层工具。

依赖只能向下：DB 协调其他模块；MemTable 不做 IO；IO 不理解 WAL；SSTable 不调用 DB；Manifest 不读取 KV 内容。

## PImpl：公开 DB 与内部实现之间的壳

PImpl 在 public header 中只保留一个实现指针：

```cpp
class DB {
 public:
  ~DB();
  Status Put(std::string_view key, std::string_view value);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
```

`DB::Impl` 在 `.cpp` 中持有 MemTable、WAL、VersionSet 等内部成员。好处是隐藏依赖、稳定 public header、减少重新编译并为 library ABI 留出空间；代价是一次堆分配、指针间接访问和额外样板代码。

TinyLSM 的顶层 `DB` 适合使用 PImpl，因为 V0 到 V2 的内部成员变化明显；`Status`、`Entry`、MemTable、WALReader 等小型或内部类型不需要 PImpl。它是特定场景技术，不是所有类的默认写法。

## 测试如何成为架构的一部分

测试分三层：

1. 单元测试：MemTable、编码、CRC、WAL reader/writer、SSTable builder/reader、Manifest。
2. Public API 集成测试：Put/Get/Delete/Scan、Close/Open、flush 后重启。
3. 恢复测试：WAL 尾部截断、中部损坏、SSTable/Manifest 发布各故障点。

测试支持设施包括：

- RAII 临时数据库目录；
- 文件截断和单字节破坏工具；
- 明确 failpoint，不用 `sleep` 猜执行时序；
- 用 `std::map` 作为参考模型，随机执行操作并比较结果；
- Debug、ASan、UBSan 和 Linux CI。

## 当前结论

- `已确认`：知识结构应围绕“一次写入如何从 API 进入 WAL/MemTable，最终发布为 SSTable”组织。
- `计划`：V0-V2 的公开顶层 `DB` 使用 PImpl；只在 public façade 使用，不扩散到内部小类。
- `建议，待确认`：V1/V2 核心 WAL/SSTable/Manifest 使用最小自定义二进制格式，不直接依赖 Protobuf。
- `计划`：测试与源码同时演进；每个版本都要求 unit、integration 和对应 recovery 证据。
- `未做`：没有创建工程骨架、源码、测试或 commit。

## 自检问题

1. 为什么 WAL 必须在 MemTable 之前成功？
2. 为什么 `write()` 返回成功仍不能等同于断电持久化？
3. 为什么 Manifest 未引用的完整 SSTable 仍不能直接使用？
4. Sequence number 与 WAL offset、文件号有什么区别？
5. 为什么 Protobuf 不能代替 WAL framing 和 checksum？
6. 为什么 SSTable 不适合每次 Put 原地修改？
7. V2 重启时哪些数据从 SSTable 读取，哪些数据从 WAL 重建？
