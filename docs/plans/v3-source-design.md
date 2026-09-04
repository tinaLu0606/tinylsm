# 2026-09-01 · TinyLSM · V3 多 SSTable 生命周期设计

## 状态

- 最近更新：2026-09-04
- 状态：`实施中（第一阶段已完成）`
- 定位：分阶段实现与验收依据

第一阶段已完成 Manifest 双版本、多表读取基础和 Open metadata 校验；重复 flush、内部 iterator、compaction 与 orphan cleanup 仍待后续阶段实现。

V3 把原路线中的多 SSTable、同步 compaction，以及 Scan 所需的内部归并 iterator 合并为一个阶段。

## 1. 目标与边界

```text
MemTable
   │ flush
   ▼
SST #1 ── SST #2 ── ... ── SST #N
   └──── Get / merge Scan ─────┘
                  │
                  │ DB::Compact()
                  ▼
          0 or 1 replacement SST
```

V3 必须完成：

- 重复 flush 和多 SSTable 重启恢复；
- MemTable + 多 SSTable 的 Get 与有序 Scan；
- value/tombstone 按 sequence 选择最新版本；
- 内部惰性 SSTable iterator 和多路归并；
- 显式同步 full compaction；
- Manifest 多表格式及 V2 单表格式读取兼容；
- flush/compaction 故障恢复。

V3 不包含：

- 自动或后台 compaction；
- partial、leveled 或 tiered compaction；
- public Iterator、snapshot、MVCC；
- 并发、WriteBatch、group commit；
- Bloom Filter、Block Cache、benchmark；
- install/package、CI 和 CLI 扩展。

## 2. 关键决定

| 问题 | 决定 |
|---|---|
| 表的顺序 | Manifest 按 oldest-to-newest 保存 |
| Get | 先 MemTable，再从 newest SST 向 oldest 查询 |
| Scan | 内部 iterator 多路归并；public API 仍返回 `vector<Entry>` |
| Compaction | 新增显式同步 `DB::Compact()`，每次合并全部 live SST |
| Tombstone | flush 保留；full compaction 才允许删除 |
| 状态管理 | 继续使用 `ManifestState`，暂不引入 `VersionSet` |
| Commit point | 新 Manifest durable |
| 编号文件名 | 十进制编号至少六位；超过六位后自然扩展，解析后必须能规范化 round-trip |
| Open metadata 校验 | V3 不升级 SSTable 格式；Open 完整读取每张 live SSTable，计算并核对真实 metadata |

暂不自动 compaction。否则 Put 的 flush 已经 durable、后续维护性 compaction 却失败时，Put 应返回成功还是失败会变得含糊。

## 3. 数据不变量

Manifest 必须满足：

- active WAL 和所有 live SSTable 都存在并可被真实 Reader 打开；
- table file number 唯一，且不与 active WAL 相同；
- `next_file_number` 大于所有已引用文件号；
- live SSTable 按 oldest-to-newest 排列；
- 相邻表的 sequence range 严格递增：

```text
table[i].max_sequence < table[i + 1].min_sequence
```

- 每张表满足 `min_sequence <= max_sequence <= last_sequence`；
- key 范围和归并统一使用 byte-wise comparator。

不能证明这些条件时，Open 返回 `Corruption`，不能猜测新旧顺序。

同一个 key 的可见记录是 sequence 最大的一条：

```text
MemTable seq=27  tombstone
SST #9   seq=21  value=new
SST #4   seq=8   value=old

结果：NotFound，旧值不能重新出现
```

Get、Scan 和 Compaction 必须使用同一规则。

## 4. 读取路径

### Get

```text
MemTable
  ├─ value     -> return value
  ├─ tombstone -> NotFound
  └─ miss
       ↓
SST newest -> oldest
  ├─ value     -> return value
  ├─ tombstone -> NotFound
  └─ all miss  -> NotFound
```

可以根据 Manifest 中的 smallest/largest key 跳过不相关的表。

### Scan

MemTable 和每张 SSTable 提供内部 iterator。多路归并器按 key 排序，对相同 key 选择最大 sequence：

- value：加入结果；
- tombstone：不输出；
- IO/CRC 错误：整个 Scan 失败，不能返回部分成功结果。

内部 iterator 惰性跨 block 读取；public `Scan()` 仍物化最终结果。

### 内部 Iterator 合同

V3 引入一个仅供存储模块使用的前向 iterator 接口，不暴露到 public API：

```cpp
class InternalIterator {
public:
  virtual ~InternalIterator() = default;

  [[nodiscard]] virtual bool Valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalEntry& entry() const = 0;
  virtual Status Next() = 0;
  [[nodiscard]] virtual const Status& status() const noexcept = 0;
};

Result<std::unique_ptr<InternalIterator>>
SSTableReader::NewIterator(std::string_view begin,
                           std::string_view end) const;

Result<std::unique_ptr<InternalIterator>>
MemTable::NewIterator(std::string_view begin,
                      std::string_view end) const;

Result<std::unique_ptr<InternalIterator>>
NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs);
```

共同语义：

- factory 成功后，iterator 已定位到 `[begin, end)` 中第一条记录；空范围返回 `Valid() == false` 且 `status().ok()`；
- 只有 `Valid() == true` 时才能调用 `entry()`，引用只保证在本 iterator 下一次 `Next()` 前有效；
- `Next()` 成功时移动到下一条或正常 EOF；IO、CRC 或格式错误返回非 OK，并把 iterator 置为 sticky error；
- error 和 EOF 时 `Valid() == false`，两者由 `status()` 区分；error 后再次 `Next()` 返回同一错误；
- iterator 不可复制、可以移动，不允许越过其借用的数据源生命周期。

`SSTableIterator` 借用 `SSTableReader`，只持有当前 block 解码后的 entries、block 下标和 entry 下标。创建时通过 index 定位第一个候选 block；`Next()` 仅在当前 block 用尽后读取下一相关 block，因此一次最多为每张表保留一个已解码 block。

`MemTableIterator` 借用 `MemTable` 的有序 map。TinyLSM 不支持并发，`DB::Scan()` 执行期间不会写 MemTable，因此 iterator 存活期间不会发生使其失效的修改。

`MergingIterator` 拥有所有输入 iterator，并用最小堆选择 byte-wise 最小 key。对于同一个 key：

1. 收集当前位于该 key 的全部输入；
2. 选择 sequence 最大的记录作为当前输出，包括 tombstone；
3. `Next()` 时推进该 key 对应的全部输入，再选择下一个 key；
4. 相同 key 出现相同 sequence 返回 `Corruption`，因为全局 sequence 和 Manifest range 合同不允许这种状态；
5. 任一输入报错时立即进入 sticky error，不再产生后续记录。

`DB::Scan()` 消费 merge iterator，将 value 复制进局部结果 vector 并过滤 tombstone。只有 iterator 正常到达 EOF 才返回 vector；中途失败时丢弃局部结果并返回错误。`Compact()` 使用同一个 merge iterator 和相同的 winner 规则，只把 value 写入 replacement SSTable。

生命周期固定为：

```text
DB owns MemTable/readers
  -> local source iterators borrow them
  -> local MergingIterator owns source iterators
  -> Scan/compaction consumes and destroys MergingIterator
  -> compaction 才能 swap readers、关闭旧 reader、清理旧文件
```

因此 compaction 必须在调用 `ManifestState::Publish()` 前完成 iterator 消费并销毁所有借用旧 Reader 的 iterator；commit 后不再保留 iterator，也不会因 table-set swap 产生悬空引用。

### Open 时核对 SSTable metadata

当前 SSTable index 只有 block 的首尾 key，没有 table-level sequence 范围。V3 不为此引入新的 SSTable format；`SSTableReader` 增加内部校验接口：

```cpp
struct SSTableProperties {
  std::uint64_t file_size;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence;
  std::uint64_t max_sequence;
};

Result<SSTableProperties> SSTableReader::ValidateAndGetProperties() const;
```

`ValidateAndGetProperties()` 完整读取一次所有 data block，验证每个 block 的 CRC、entry 顺序、index 首尾 key 与实际 block 首尾 key一致、跨 block key 严格递增、sequence 大于 0，并计算真实 properties。空 SSTable 返回 `Corruption`。

`DB::Open()` 对每个 `live_tables` 项按以下顺序执行：

1. 核对文件存在和实际 file size；
2. `SSTableReader::Open()` 校验 footer 与 index；
3. `ValidateAndGetProperties()` 完整校验数据并计算 properties；
4. 将 file size、smallest/largest key、min/max sequence 与 Manifest 精确比较；
5. 所有 Reader 和 vector 容量准备成功后，才一次性装入 `tables_`。

任一步失败都返回 `Corruption` 或带上下文的 IO error，且不能留下部分打开的 DB。完成上述校验前不能使用 Manifest key range 跳表。

这会让 V3 的 Open 成本成为 `O(全部 live SSTable bytes)`。对当前学习型、无 benchmark 目标的版本优先保证 metadata 可验证；以后若启动成本成为实际问题，再通过带 checksum 的 SSTable properties block 升级格式，而不是在 V3 同时扩张两套持久化格式。

## 5. Flush 与 Compaction

重复 flush：

```text
build/sync N.sst.tmp
  -> rename N.sst + SyncDir
  -> create/sync replacement WAL
  -> prepare next table set
  -> publish Manifest                 <- commit point
  -> switch WAL/table set
  -> clear MemTable
  -> best-effort remove old WAL
```

新 Manifest 保存 `old live_tables + new table`。

V3 增加：

```cpp
Status DB::Compact();
```

`Compact()` 合并全部已发布 SSTable，不 flush/修改 MemTable，也不更换 active WAL：

```text
merge all SST iterators
  -> keep newest record per key
  -> drop tombstone
  -> build replacement SST
  -> publish Manifest                 <- commit point
  -> swap table set
  -> best-effort remove old SSTs
```

full compaction 覆盖全部磁盘表，因此可以安全删除 tombstone。特殊情况：

- 零张表：返回 OK；
- 一张表：允许重写；
- 结果全是 tombstone：发布零张表，不制造空 SSTable。

Compaction 的文件编号与状态变化合同：

- 非空输出使用 `current.next_file_number` 创建 replacement SSTable，成功发布后把 `next_file_number` 加一；
- builder 在遇到第一条 value 时才延迟创建，因此全 tombstone 结果不创建临时/空 SSTable，也不消耗 file number；
- `active_wal_number`、`last_sequence`、`next_sequence_`、MemTable 和当前 WAL handle 全部保持不变；
- compaction 路径禁止创建 replacement WAL；replacement WAL 只属于 Flush；
- active WAL 中比磁盘表更新的 value/tombstone 继续由 MemTable 覆盖 compacted table。

## 6. Manifest V3 格式

### 6.1 方案选择

V3 继续使用“完整快照文件 + temp/rename/SyncDir”，不改成 LevelDB/RocksDB 的 append-only VersionEdit log。

| 参考方案 | 特点 | 对 TinyLSM V3 的判断 |
|---|---|---|
| LevelDB | MANIFEST 是带 checksum 的 record log，追加 `VersionEdit`，由 `CURRENT` 指向当前 MANIFEST | 适合大量版本、level 和增量修改；V3 引入会同时需要 log replay、rolling 和 CURRENT 协议 |
| RocksDB | 在 VersionEdit log 上增加 atomic group、rolling MANIFEST 和更复杂的恢复 | 面向并发 compaction 与大规模元数据，超出 V3 范围 |
| Mini-LSM | 采用 append-only manifest record，并指出完整快照在约万张表时会变慢 | 说明 snapshot 的扩展性边界，但不意味着小型 V3 必须立即使用 edit log |
| TinyLSM V3 | 每次发布完整 `ManifestSnapshot` | 当前单线程、同步 full compaction、无 level/version 生命周期，恢复最简单 |

因此 V3 保持 snapshot 设计，接受每次发布重写 `O(live_tables)` metadata。以后出现自动/后台 compaction、多个 level 或数千张表时，再迁移到 `MANIFEST-N + CURRENT + VersionEdit`，不在 V3 预埋半套日志系统。

参考：

- [LevelDB VersionEdit 编码](https://github.com/google/leveldb/blob/main/db/version_edit.cc)
- [LevelDB VersionSet 发布与恢复](https://github.com/google/leveldb/blob/main/db/version_set.cc)
- [RocksDB MANIFEST 设计](https://github.com/facebook/rocksdb/wiki/MANIFEST)
- [Mini-LSM Manifest 教程](https://skyzh.github.io/mini-lsm/week2-05-manifest.html)
- [Protobuf proto3 兼容规则](https://protobuf.dev/programming-guides/proto3/#updating)

### 6.2 外层 framing

V3 沿用 16-byte little-endian header：

| Offset | Size | 字段 | V3 值与含义 |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x31464e4d`，磁盘字节为 `MNF1`；作为历史 container magic 保持不变 |
| 4 | 2 | `format_version` | `2` |
| 6 | 2 | `flags` | 必须为 `0`；非零返回 `Corruption` |
| 8 | 4 | `payload_length` | Protobuf payload 字节数 |
| 12 | 4 | `checksum` | CRC32C，规则见下文 |
| 16 | N | `payload` | `ManifestSnapshotProto` |

```text
offset 0                                                    offset 16
| magic | version=2 | flags=0 | payload_length | CRC32C |
| Protobuf payload (payload_length bytes)                   |
```

format version 1 的 checksum 只覆盖 payload。format version 2 改为：

```text
CRC32C(bytes[0..12] || payload)
```

它覆盖 magic、version、flags、payload length 和 payload，但不覆盖 checksum 字段本身。decoder 按读到的 version 选择对应 checksum 规则。

共同限制：

- 文件至少 16 bytes；
- `payload_length == file_size - 16`，不允许尾随字节；
- Encode 和 Decode 使用相同的 128 MiB 完整文件上限；
- Encode 超限返回 `ResourceExhausted`，Decode 超限返回 `Corruption`；
- checksum 验证存储的实际 bytes，不依赖 Protobuf deterministic serialization。

### 6.3 Protobuf payload

V3 不新增 field 5。原 field 4 直接从单个 message 改为 repeated message：

```proto
syntax = "proto3";

package tinylsm.proto;

message TableMetaProto {
  uint64 file_number = 1;
  uint64 file_size = 2;
  bytes smallest_key = 3;
  bytes largest_key = 4;
  uint64 min_sequence = 5;
  uint64 max_sequence = 6;
}

message ManifestSnapshotProto {
  uint64 active_wal_number = 1;
  uint64 next_file_number = 2;
  uint64 last_sequence = 3;
  repeated TableMetaProto live_tables = 4; // oldest-to-newest
}
```

选择 field 4 的原因：

- Protobuf field number 是 wire identity，不能随意重编号或复用；
- message 类型的 singular 与 repeated 在 wire 上都是 length-delimited；V2 的零或一个 field 4 可由新 schema 读成零或一项；
- repeated message 保留元素出现顺序，适合表达 oldest-to-newest；
- 旧 schema 读取多个 field 4 时可能合并 message，因此 V3 writer 必须写外层 version 2，让旧 TinyLSM 在解析 payload 前拒绝。

这比“field 4 保留旧表、field 5 新增表数组”更简单：少一套字段、没有双字段冲突状态，同时保持安全的单向升级。

字段合同：

| 字段 | 合同 |
|---|---|
| `active_wal_number` | 大于 0，且不与 live table file number 相同 |
| `next_file_number` | 大于 active WAL 和所有 live table file number |
| `last_sequence` | 已发布 SSTable 的 sequence 上界；初始数据库可为 0 |
| `live_tables` | 可为空；严格按 oldest-to-newest；file number 不重复 |
| `file_number` | 大于 0 |
| `file_size` | 大于 0，并与实际 SSTable 大小一致 |
| `smallest_key/largest_key` | 任意 bytes；按 `BytewiseLess` 满足 smallest <= largest |
| `min_sequence/max_sequence` | 均大于 0，且 min <= max <= last_sequence |

相邻 table 必须满足：

```text
live_tables[i].max_sequence < live_tables[i + 1].min_sequence
```

Open 使用 smallest/largest 跳过 SSTable 前，必须把 Manifest 边界与 SSTable index 的首尾 key 交叉验证；否则可解析但错误的 metadata 可能让 Get 跳过真实包含 key 的表。

### 6.4 编码与解码

```text
Encode:
ValidateSnapshot
  -> serialize Protobuf
  -> enforce 128 MiB limit
  -> encode first 12 header bytes
  -> CRC32C(header[0..12] + payload)
  -> append checksum and payload

Decode:
check total size
  -> validate magic/version/flags/exact payload length
  -> verify version-specific CRC32C
  -> parse complete Protobuf payload
  -> reject unknown top-level or nested fields
  -> validate snapshot/table invariants
  -> DB::Open verifies referenced files and SSTable boundaries
```

V3 对未知字段返回 `Corruption`。外层 version 是格式协商机制；未来增加有语义的字段时应提升 version，而不是依赖旧程序静默忽略。

### 6.5 兼容矩阵

| 磁盘文件 | V3 reader | V2 reader |
|---|---|---|
| version 1，零/一张表 | 按旧 checksum 验证；field 4 读成零/一项 | 正常读取 |
| version 1，多个 field 4 | `Corruption`；旧 writer 不可能合法产生 | 不视为合法文件 |
| version 2，多张表 | 正常读取 | 在解析 Protobuf 前因 version 不支持而拒绝 |
| 未知 version | `Corruption` | `Corruption` |

仅 Open version 1 文件时不改写。下一次成功 flush 或 compaction 发布完整 version 2 snapshot。pre-Manifest 非空 WAL 仍返回 `NotSupported`，不属于本次升级。

### 6.6 发布与测试

发布顺序保持不变：

```text
Encode version 2 snapshot
  -> write MANIFEST.tmp
  -> file Sync + Close
  -> rename MANIFEST.tmp -> MANIFEST
  -> SyncDir                         <- durable
```

必须增加：

- 固定 format-v1 golden bytes，证明新 decoder 不依赖新 encoder 也能读取旧格式；
- format-v2 的零/一/多 table round-trip 和 repeated 顺序测试；
- magic、version、flags、length、checksum、payload 分别损坏的测试；
- format-v1 多个 field 4、format-v2 unknown field、重复 file number、sequence range 重叠的拒绝测试；
- Encode/Decode 共同的 128 MiB 边界测试；
- format-v2 Manifest 被旧 version gate 拒绝的兼容测试；
- metadata 与 SSTable size、首尾 key 不一致时，真实 Open 返回 `Corruption`；
- 保留 temp write/sync/close/rename/SyncDir 的故障注入测试。

## 7. 提交与恢复

```text
Manifest commit 前失败
  -> 旧 Manifest 有效，新文件是 orphan

Manifest durable
  -> 新状态生效，旧文件是 orphan

Manifest visible 但目录 sync 失败
  -> 当前 handle 进入 terminal state
```

所有可能失败的 reader 打开、metadata 构造和 vector 扩容必须在 commit 前完成。commit 后只做不抛异常的 swap/move 和 best-effort cleanup，避免磁盘与当前 handle 状态分裂。

## 8. 实现顺序

原则：每一步同时实现对应测试并保持仓库可编译、可运行；不能等功能全部完成后再集中补测试，也不能先写出当前 DB 无法读取的新状态。

1. **已完成（2026-09-04）**：Manifest 多表 schema、format-v1 golden fixture、双版本读取和不变量测试；`ManifestSnapshot::live_table` 与 `DB::Impl::table_` 已改为 vector，live SSTable properties 会在 Open 时完整校验；DB 已能读取零到多张表，并暂时保留第二次 flush 限制；
2. 完成重复 flush、多表 Get 和 WAL sequence 恢复校验；测试每次 flush 后的当前读取、关闭重开以及 value/tombstone 覆盖；
3. 按第 4 节合同实现 MemTable/SSTable iterator、多路归并和完整 Scan；测试同 key 去重、范围边界、sticky error 以及后续 block 失败时不返回部分结果；
4. 实现同步 `DB::Compact()`；提交前准备好 replacement Reader、Manifest metadata 和 vector 容量，提交后只做不抛异常的 swap/move 与 best-effort cleanup；同时覆盖零表、一表、全 tombstone 和各提交阶段失败；
5. 实现共享 filename parser 与 `CleanupObsoleteFiles()`，扩展 `ListDir`、随机读等故障注入能力；测试误删保护、删除失败重试和文件编号边界；
6. 运行跨模块故障矩阵和 V0-V2 全量回归，更新 README，再执行全部构建、格式、lint 和 sanitizer 验证。

## 9. 验收重点

Manifest 与恢复：

- 使用旧实现产生的固定 format-v1 bytes 验证零/一张表兼容，不能由新 encoder 临时生成旧 fixture；
- format-v2 零/一/多表 round-trip，未知 version、非法字段、重复文件号和重叠 sequence range 返回 `Corruption`；
- 引用文件缺失，或 Manifest 的 size、首尾 key、sequence metadata 与真实 SSTable 冲突时返回 `Corruption`；
- active WAL 的 sequence 必须严格递增且大于已发布 `last_sequence`；允许合法空洞，拒绝重复和倒退；

多表读写与 iterator：

- 连续三次以上 flush，并在每次 flush 后及重启后验证结果；
- 同一 key 在 MemTable 和三张以上 SSTable 中交替出现 value/tombstone 时，Get 与 Scan 都选择最大 sequence；
- NUL、高位字节、空 key、跨 block 范围和 `[begin, end)` 边界保持 byte-wise 顺序；
- iterator 在后续 block 遇到 IO/CRC 错误时，整个 Scan 失败且不返回部分结果；

Compaction 与提交：

- compaction 前后、重启前后 Get/Scan 逻辑结果等价；
- 覆盖零表、一表、结果全为 tombstone、in-memory、closed 和 terminal-state；
- Flush 的 SSTable、replacement WAL、Manifest 在 commit 前失败时恢复旧状态，Manifest durable 后恢复新状态，visible-not-durable 进入 terminal state；
- Compaction 只覆盖 replacement SSTable 与 Manifest 的失败阶段；测试必须确认它不创建或更换 WAL，且 `active_wal_number` 保持不变；
- Manifest commit 后的 Reader/vector 切换不再分配内存或执行可失败的正确性步骤；

Cleanup 与回归：

- 扩展故障注入以覆盖 `ListDir`、`OpenRandomAccess`/`ReadAt`、`Remove` 和 `SyncDir`；
- commit 前 orphan、旧 WAL、compaction 输入表删除失败后可在当前进程或下次 Open 重试；active 文件和无关文件绝不删除；
- filename parser 接受 `000001`、`999999`、`1000000`，拒绝零、整数溢出、非法后缀和不能规范化 round-trip 的名称；
- 连续 flush/compaction 后目录只保留 Manifest 引用文件及允许的无关文件；
- V0-V2 回归测试全部通过。故障注入验证系统调用失败处理，不宣称等价于真实断电测试。

最终验证：

```text
./run test
./run asan
./run format --check
./run lint dev-debug

cmake --preset release -DBUILD_TESTING=ON
cmake --build --preset release
ctest --test-dir build/release --output-on-failure

git diff --check
```

V3 完成后，再安排 install/package，并重新讨论并发、自动 compaction 和缓存等后续方向。

## 10. Orphan 安全清理计划

### 风险判断

正常文件系统上，关闭旧 WAL/SSTable 后删除失败的概率较低。但 V3 支持重复 flush 和 compaction，运行次数不再有上限；进程也可能恰好在 Manifest 提交后、删除旧文件前崩溃。因此单次低概率不能保证生命周期内不累积，应该在 V3 一并处理。

### 编号文件名规范

共享 filename 模块替代 `db_impl.cpp` 中只接受六位数字的 `IsNumberedFile()`，同时提供 format 和 parse，供 Open、Flush、Compaction 与 Cleanup 使用。

规范名称为：

```text
<canonical-decimal>.wal
<canonical-decimal>.sst
<canonical-decimal>.sst.tmp
```

其中 `<canonical-decimal>` 的规则是：

- file number 必须在 `[1, UINT64_MAX]`；
- `1..999999` 左侧补零到六位，例如 `1 -> 000001`；
- 从 `1000000` 开始使用不带前导零的自然十进制，位数不设六位上限；
- parser 只接受 ASCII 数字，必须完整消费数字部分并检查 `uint64_t` 溢出；
- parse 后重新 format 必须与原文件名逐字节相同，否则视为非规范名称，不参与自动清理。

示例：

| 名称 | 结果 |
|---|---|
| `000001.sst` | 接受，编号 1 |
| `999999.wal` | 接受，编号 999999 |
| `1000000.sst.tmp` | 接受，编号 1000000 |
| `1.sst`、`0000001.sst` | 拒绝，不能规范化 round-trip |
| `000000.wal` | 拒绝，编号 0 非法 |
| `+000001.sst`、含空白名称 | 拒绝，只允许 ASCII 数字 |
| 超过 `UINT64_MAX` 的十进制 | 拒绝，整数溢出 |

Cleanup 只删除 parser 成功识别且确认不在 Manifest live set 中的文件。解析失败的近似名称和无关文件一律保留。

### 清理规则

新增私有的 `CleanupObsoleteFiles()`：

1. 只有 Manifest、全部 live SSTable 和 active WAL 成功恢复后才运行；
2. 从 Manifest 建立 live file set；
3. 只识别共享 filename parser 能解析并 round-trip 的编号 `.wal`、`.sst`、`.sst.tmp`，以及 `MANIFEST.tmp`；
4. 删除其中未被 live file set 引用的文件；
5. 不识别或无关的文件绝不删除；
6. 删除成功后 best-effort `SyncDir`；清理失败不改变 Open/flush/compaction 已提交结果。

执行时机：

- Open 完整恢复成功后扫描一次，清理由崩溃或上次进程遗留的 orphan；
- flush/compaction durable 后，关闭旧 handle，再立即尝试删除对应旧文件；
- 删除失败的路径保存在内存 pending 列表，在下一次 flush/compaction 时重试；
- 重启后即使 pending 列表丢失，也会由 Open 扫描重新发现。

安全边界：

- Open 失败或 Manifest 损坏时不清理，保留现场；
- Manifest 引用的文件即使看起来陈旧也不能删除；
- 正确性仍依赖 Manifest 忽略 orphan，不能依赖清理一定成功；
- 永久权限或磁盘故障仍可能阻止删除，但测试必须排除 TinyLSM 自身持续误产 orphan。

验收测试：

- commit 前产生的临时/正式 orphan 在下次 Open 后被清理；
- 旧 WAL 和 compaction 输入表删除失败后会被重试；
- active WAL、live SSTable 和无关文件始终保留；
- filename parser 拒绝非规范名称、数字溢出和近似后缀；
- cleanup 的 `ListDir`、`Remove`、`SyncDir` 失败不破坏已恢复数据；
- 连续多次 flush/compaction 后，目录只保留 Manifest 引用文件及允许的无关文件。
