# 2026-08-31 · TinyLSM V0-V2 源码理解考题

## 说明

- 类型：项目源码理解检查
- 状态：`未作答`
- 整理方式：AI 根据当前 TinyLSM V0-V2 源码整理
- 目标：确认是否理解项目目标、主要模块、核心调用流程和当前版本边界
- 难度：以读懂现有源码为主，不要求提前设计 V3

这是开卷考察，可以一边阅读源码一边回答。每题用自己的话回答 3-6 句话即可；
不要求背二进制字段偏移，也不要求写代码。

回答时尽量包含：

```text
1. 这段代码或这个模块要解决什么问题？
2. 它和哪些模块发生联系？
3. 当前 V2 做到了什么，又没有做什么？
```

## 第一组：项目目标与整体结构

### 1. TinyLSM 这个项目的主要目标是什么？

请说明它希望帮助你学习什么。为什么当前 TinyLSM 应被称为学习型原型，而不是
生产数据库？

参考：`docs/plans/v0-v2-source-design.md`

### 2. V0、V1、V2 分别增加了什么能力？

用一两句话分别概括三个阶段。重点说明数据从“只在内存”到“可以恢复”，再到
“可以 flush 到一个 SSTable”的变化。

### 3. 当前对外提供了哪些主要操作？

说明 `OpenInMemory`、`Open`、`Put`、`Get`、`Delete`、`Scan` 和 `Close` 各自做
什么。为什么 `Get` 需要 `Result<std::string>`，不能只返回 `std::string`？

参考：`include/tinylsm/db.h`

### 4. WAL、MemTable、SSTable、Manifest 各自负责什么？

请分别用一句话说明四个组件的职责。再说明哪一个组件记录“当前哪些文件有效”。

### 5. `DB::Impl` 在项目中扮演什么角色？

为什么 Put、Open、Get 和 flush 的整体流程放在 `DB::Impl` 中协调，而不是让 WAL、
MemTable 和 SSTable 彼此直接调用？

参考：`src/db/db_impl.h`、`src/db/db_impl.cpp`

## 第二组：沿着源码理解读写流程

### 6. 一次 Put 大致经过哪些步骤？

不要求说出每个判断，只需按顺序说明：检查输入、写 WAL、按配置 Sync、更新
MemTable，以及 MemTable 足够大时可能发生什么。

参考：`DB::Impl::Put()`、`DB::Impl::Write()`

### 7. 为什么要先写 WAL，再更新 MemTable？

如果反过来先更新 MemTable，然后 WAL 写入失败，会出现什么问题？这和数据库重启
恢复有什么关系？

### 8. Get 为什么先查 MemTable，再查 SSTable？

请分别说明 MemTable 中找到普通 value、找到 tombstone、完全找不到 key 时，Get
接下来会怎么做。

参考：`DB::Impl::Get()`

### 9. Delete 为什么写入 tombstone，而不是立即从所有地方删除 key？

说明 tombstone 如何遮住 SSTable 中可能存在的旧 value。为什么删除一个不存在的
key 也可以返回成功？

### 10. Scan 如何得到最终结果？

说明当前实现如何组合 SSTable 和 MemTable 中的数据、同一个 key 以谁为准，以及
为什么最终结果中不包含 tombstone。

参考：`DB::Impl::Scan()`、`MemTable::Scan()`、`SSTableReader::Scan()`

## 第三组：持久化、恢复与当前边界

### 11. 重新打开数据库时，大致发生什么？

按高层顺序说明：检查目录、加载或创建 Manifest、打开 Manifest 引用的 SSTable、
回放活跃 WAL，最后恢复成可以继续读写的状态。

参考：`DB::Impl::Open()`

### 12. Manifest 为什么是有效文件集合的权威来源？

如果目录中存在一个 Manifest 没有引用的 WAL 或 SSTable，当前代码为什么不应该
擅自使用它？这些文件可能是怎样留下来的？

### 13. 第一次 flush 做了什么？

用高层流程说明：把 MemTable 写成临时 SSTable、验证并发布 SSTable、创建新 WAL、
发布新 Manifest、切换内存状态、清理旧 WAL。哪个步骤完成后，新文件集合才正式
生效？

参考：`DB::Impl::FlushMemTable()`、`ManifestState::Publish()`

### 14. 当前 V2 有哪些明确限制？

至少列出四项，例如 SSTable 数量、compaction、线程安全、Iterator、后台 flush、
真实断电验证等。为什么第二次需要 flush 时会返回 `NotSupported`？

### 15. V3 最自然的下一步是什么？

不需要设计具体实现。只需说明：为什么支持多个 SSTable 是 V2 之后自然的下一步；
它会让 Get、Scan 和 Manifest 比现在多考虑什么问题；哪些工作仍然可以留给后续的
compaction 阶段？

## 简单评估标准

每题 0-2 分，总分 30 分：

- 0 分：没有理解核心意思，或答案与当前源码相反；
- 1 分：大方向正确，但模块关系或执行顺序比较模糊；
- 2 分：能用自己的话讲清目的、主要流程和当前版本边界。

可以继续讨论 V3 的参考条件：

1. 总分达到 22/30 左右；
2. 第 1、4、6、11、14 题的大方向正确；
3. 能说清 WAL、MemTable、SSTable、Manifest 的职责；
4. 知道 V2 目前只有一个已发布 SSTable，没有 compaction 和并发承诺。

不需要一次回答得非常完整。如果某个流程说不清楚，先回到对应函数一起走一遍调用
链，再继续回答即可。重点是逐渐建立对整个项目的地图，而不是一次考试定结果。
