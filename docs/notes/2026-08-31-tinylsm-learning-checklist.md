# 2026-08-31 · TinyLSM 系统基础学习清单

## 说明

- 类型：技术学习路线
- 状态：`未做`；勾选表示自己已经能够解释、定位代码并完成小实验，而不只是看过
- 目标：掌握读懂和继续实现 TinyLSM 所必需的系统性基础
- 配套笔记：[`2026-08-24-tinylsm-v0-v2-foundations.md`](2026-08-24-tinylsm-v0-v2-foundations.md)
- 整理方式：AI 根据当前 TinyLSM 代码结构整理

不必一次学完。建议按下面的顺序推进，每次只学习一个主题，并立刻回到项目中找对应代码。

## 第一阶段：能读懂项目中的现代 C++

- [ ] 理解对象生命周期、构造与析构，以及 RAII 为什么适合管理文件资源。
  - 代码入口：`src/io/file.h`、`src/db/db.cpp`
  - 学会标准：能解释数据库或文件对象离开作用域时发生什么。
- [ ] 理解值、引用、指针和所有权的区别。
  - 重点类型：`T`、`T&`、`const T&`、`T*`、`std::unique_ptr<T>`
  - 学会标准：能解释 `DB` 为什么不可复制但可以移动。
- [ ] 掌握移动语义和 `std::move`。
  - 代码入口：`DB::Open`、`DB::Impl::Open`、各 Reader/Writer 构造函数
  - 学会标准：能说清文件对象的所有权从哪里转移到哪里。
- [ ] 掌握 `std::string`、`std::string_view` 和 `std::span` 的差别。
  - 学会标准：知道谁拥有内存，并能识别悬空 view/span 的风险。
- [ ] 理解 `std::optional`、`enum class`、模板和 `Result<T>`。
  - 代码入口：`include/tinylsm/result.h`、`include/tinylsm/status.h`
  - 学会标准：能独立读懂 `Result<std::unique_ptr<DB>>`。
- [ ] 理解头文件、源文件、翻译单元和链接过程。
  - 配套笔记：[`2026-08-27-cpp-header-source-organization.md`](2026-08-27-cpp-header-source-organization.md)
  - 学会标准：看到函数声明时，知道如何查找定义和调用者。

## 第二阶段：错误处理与资源安全

- [ ] 区分预期错误与意外异常。
  - 预期错误：`Status`、`Result<T>`
  - 意外异常：`std::bad_alloc`、标准库或依赖抛出的异常
  - 学会标准：知道什么错误应返回，什么异常可以继续向上传播。
- [ ] 理解异常传播、栈展开、`catch` 和 `noexcept`。
  - 代码入口：`tools/tinylsm_cli.cpp`
  - 学会标准：能解释 CLI 为什么在最外层捕获异常。
- [ ] 学会给错误补充上下文，同时保留稳定的错误类别。
  - 代码入口：`Status::WithContext`、`DB::Impl::Open`
- [ ] 理解“某一步失败后，系统状态可能已经改变”。
  - 代码入口：`DB::Impl::Write`、`DB::Impl::FlushMemTable`
  - 学会标准：不再把“返回错误”简单理解为“什么都没发生”。
- [ ] 理解析构清理与显式 `Close()` 的差别。
  - 学会标准：知道为什么需要显式 `Close()` 才能观察关闭错误。

## 第三阶段：Linux/POSIX 文件与持久化语义

- [ ] 理解文件描述符以及 `open/read/write/close` 的基本语义。
- [ ] 理解短读、短写、系统调用失败和 `errno`。
  - 代码入口：`src/io/file.cpp`
  - 学会标准：能解释为什么一次 `read`/`write` 不保证处理全部字节。
- [ ] 区分用户态缓冲、操作系统 page cache 和持久化介质。
- [ ] 理解 `write`、`flush`、`fsync` 和目录 `fsync` 的区别。
  - 学会标准：能解释文件内容持久化与文件名持久化为什么是两件事。
- [ ] 理解同一文件系统内 `rename` 的原子可见性，以及它不等于持久化。
  - 代码入口：`src/manifest/manifest_state.cpp`
- [ ] 区分进程崩溃、操作系统崩溃和机器断电。
  - 学会标准：能够说明当前测试验证了哪些故障、没有验证哪些故障。

## 第四阶段：二进制格式与数据完整性

- [ ] 理解字节、整数宽度、大小端和固定宽度编码。
  - 代码入口：`src/util/coding.h`、`src/util/coding.cpp`
- [ ] 理解 framing：如何通过长度字段识别一条变长记录。
  - 代码入口：`src/wal/wal_record_codec.cpp`
- [ ] 理解 CRC32C 校验的对象和能力边界。
  - 它能发现意外字节损坏，但不是加密签名，也不能修复数据。
  - 代码入口：`src/util/crc32c.cpp`
- [ ] 理解为什么解码时必须先做长度、边界和溢出检查，再分配内存。
- [ ] 理解 Protobuf 只负责序列化 payload，不负责原子发布、`fsync` 或文件恢复协议。
  - 代码入口：`proto/manifest.proto`、`src/manifest/manifest_codec.cpp`
- [ ] 能手工画出 WAL record、SSTable block/footer 和 Manifest 的字节布局。

## 第五阶段：数据结构与查找算法

- [ ] 理解有序映射及其基本复杂度。
  - 代码入口：`src/memtable/memtable.h`
  - 学会标准：知道 MemTable 为什么能直接按 key 顺序扫描。
- [ ] 理解 comparator 和严格弱序。
  - 代码入口：`src/util/bytewise_less.h`
  - 学会标准：能解释 byte-wise ordering 与普通字符比较可能有什么差异。
- [ ] 掌握二分查找和有序区间 `[begin, end)`。
  - 代码入口：`src/sstable/sstable_reader.cpp`
- [ ] 掌握两个有序序列的归并。
  - 代码入口：`DB::Impl::Scan`
  - 学会标准：能解释内存和磁盘出现相同 key 时应该保留哪个版本。
- [ ] 能分析点查、范围扫描、写入和 flush 的时间及空间复杂度。

## 第六阶段：LSM-tree 核心原理

- [ ] 理解 LSM-tree 解决的主要问题：把随机小写入转换为追加写和批量顺序写。
- [ ] 能分别说明 WAL、MemTable、SSTable 和 Manifest 的职责，不能只背执行顺序。
- [ ] 理解 sequence number 是逻辑版本，不是时间戳或文件编号。
- [ ] 理解 tombstone 为什么必须覆盖磁盘中的旧值。
- [ ] 理解 SSTable 为什么创建后保持不可变，以及这如何简化并发读取和恢复。
- [ ] 理解 flush 与普通流 `flush()` 的区别。
- [ ] 理解 compaction 的目的：合并文件、清理旧版本和安全删除 tombstone。
- [ ] 理解写放大、读放大和空间放大之间的权衡。
- [ ] 能解释当前 TinyLSM 为什么在第二次需要 flush 时返回 `NotSupported`。

## 第七阶段：恢复协议与正确性

- [ ] 理解 Manifest 为什么是有效文件集合的权威来源。
- [ ] 理解 commit point：提交点之前和之后，哪套文件状态有效。
  - 代码入口：`DB::Impl::FlushMemTable`、`ManifestState::Publish`
- [ ] 理解临时文件、`rename`、目录同步和旧文件清理的正确顺序。
- [ ] 理解 WAL 尾部截断与 WAL 中部损坏为什么应采用不同处理方式。
- [ ] 理解恢复操作为什么需要可重复执行，并能安全忽略 orphan 文件。
- [ ] 理解 Manifest 已可见但目录同步失败时，为什么当前句柄必须停止继续读写。
- [ ] 能为一次 `Put`、一次 flush 和一次重启分别写出关键不变量。

建议至少掌握以下三个不变量：

```text
1. MemTable 中可见的持久化写入，必须能从 WAL 或 SSTable 恢复。
2. Manifest 只引用已经完整写入并验证过的文件。
3. 删除标记必须持续遮蔽更旧的 value，直到 compaction 能证明它可被移除。
```

## 第八阶段：构建、调试与测试

- [ ] 理解预处理、编译、链接，以及静态库和可执行文件的关系。
  - 代码入口：`CMakeLists.txt`、`src/CMakeLists.txt`、`tools/CMakeLists.txt`
- [ ] 理解 CMake target、include path、link dependency 和 preset。
  - 学会标准：能解释 `./run build` 最终调用了什么。
- [ ] 掌握 Debugger 的断点、单步、调用栈和变量检查。
- [ ] 掌握 AddressSanitizer 和 UndefinedBehaviorSanitizer 的用途。
  - 验证命令：`./run asan`
- [ ] 理解单元测试、集成测试、恢复测试和故障注入测试各自验证什么。
- [ ] 能为文件操作的每一个失败点设计测试，并检查失败后的数据库状态。
- [ ] 学会用 `git diff`、`git status` 和小步 commit 控制改动范围。

## 暂时不必优先学习

以下内容有价值，但不是继续理解当前 TinyLSM 的前置条件：

- 高级模板元编程和复杂 C++ Concepts
- 无锁数据结构
- 分布式一致性算法（Raft/Paxos）
- 网络协议与数据库服务端开发
- 查询优化器和 SQL 执行引擎
- 完整的多线程 LSM 后台调度系统

应先把单进程、单线程、单 SSTable 下的写入、提交和恢复逻辑真正弄清楚，再扩展这些主题。

## 推荐执行节奏

每完成一个小主题，做下面四件事：

1. 用自己的话写出一句定义。
2. 在 TinyLSM 中找到一处对应代码。
3. 画出一次真实调用或状态变化。
4. 修改一个小实验或测试来验证理解。

能够同时做到“讲清楚、找得到、画得出、测得过”，才算真正掌握。
