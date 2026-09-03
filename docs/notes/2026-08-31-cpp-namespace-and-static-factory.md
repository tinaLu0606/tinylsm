# 2026-08-31 · C++ namespace、类作用域与静态工厂函数

## 记录来源

- 类型：技术学习
- 来源：阅读 `src/db/db_impl.cpp` 中的命名空间和
  `DB::Impl::OpenInMemory()` 时产生的问题，AI 整理
- 状态：`已验证`；结论基于当前 TinyLSM 源码
- 相关文件：[`../../include/tinylsm/db.h`](../../include/tinylsm/db.h)、
  [`../../src/db/db_impl.h`](../../src/db/db_impl.h)、
  [`../../src/db/db_impl.cpp`](../../src/db/db_impl.cpp)

## namespace 的作用

`namespace` 为类型和函数提供名字作用域，主要用于组织代码并避免不同库之间的
同名冲突。例如以下类型可以同时存在：

```cpp
tinylsm::DB
leveldb::DB
rocksdb::DB
```

`db_impl.cpp` 的主体位于：

```cpp
namespace tinylsm {
// ...
} // namespace tinylsm
```

因此文件中的：

```cpp
Result<std::unique_ptr<DB::Impl>> DB::Impl::OpenInMemory();
```

完整名字可以理解为：

```cpp
tinylsm::Result<std::unique_ptr<tinylsm::DB::Impl>>
tinylsm::DB::Impl::OpenInMemory();
```

不同头文件和源文件中的同名 `namespace tinylsm` 属于同一个命名空间，但
namespace 不会代替 `#include`：使用其他文件中的声明时仍需包含对应头文件。

## 三种容易混淆的作用域

当前代码中可以看到三种不同概念：

```text
tinylsm              项目的具名命名空间
tinylsm::internal    项目内部多个文件共享的具名命名空间
namespace { ... }    仅当前翻译单元使用的匿名命名空间
DB::Impl::           类与成员函数的嵌套作用域，不是 namespace
```

### 匿名命名空间

`db_impl.cpp` 将这些辅助函数放在匿名命名空间：

```cpp
namespace {
std::string Numbered(...);
std::string WalName(...);
std::string SstName(...);
bool IsNumberedFile(...);
internal::DecodeLimits Limits(...);
} // namespace
```

这些函数只服务于当前 `db_impl.cpp`，其他翻译单元不能通过
`tinylsm::WalName()` 调用它们。另一个 `.cpp` 即使定义同名 `WalName()`，也不会
与这里发生链接名称冲突。

### `tinylsm::internal`

`internal::WalReader` 的完整名字是：

```cpp
tinylsm::internal::WalReader
```

它和匿名命名空间不同：`internal` 有名字，可以让 TinyLSM 内部的多个文件共享
类型和函数；“internal”主要表达项目设计意图，并不从语言层面阻止外部代码访问。

### `DB::Impl::`

`DB::Impl::OpenInMemory` 中的 `::` 表示逐层进入作用域：

```text
tinylsm          namespace
`-- DB           类
    `-- Impl     DB 中声明的嵌套类
        `-- OpenInMemory()  Impl 的静态成员函数
```

所以 `DB::Impl::` 不是命名空间，而是类作用域。

## `OpenInMemory()` 是工厂函数，不是构造函数

当前实现是：

```cpp
Result<std::unique_ptr<DB::Impl>> DB::Impl::OpenInMemory() {
  return std::unique_ptr<Impl>(new Impl());
}
```

真正的构造函数是 `db_impl.h` 中的：

```cpp
private:
  Impl() = default;
```

`OpenInMemory()` 是一个静态工厂函数。它调用私有构造函数创建对象，但它本身不是
构造函数。执行过程可以拆成：

```text
new Impl()
    | 创建一个 DB::Impl 对象
    v
std::unique_ptr<Impl>
    | 接管对象所有权，负责自动析构
    v
Result<std::unique_ptr<DB::Impl>>
    | 表示成功返回对象，或者失败返回 Status
    v
返回调用者
```

概念上可以展开为：

```cpp
Impl* raw_pointer = new Impl();
std::unique_ptr<Impl> pointer(raw_pointer);
Result<std::unique_ptr<Impl>> result(std::move(pointer));
return result;
```

实际代码没有保留裸指针变量，而是立即把 `new Impl()` 的结果交给
`std::unique_ptr`，从而避免手动 `delete`。

## 为什么这里没有使用 `std::make_unique`

直觉上可能想写：

```cpp
return std::make_unique<Impl>();
```

但 `Impl()` 是私有构造函数。`OpenInMemory()` 作为 `Impl` 的成员函数，有权限在
自己的函数体内直接执行 `new Impl()`；`std::make_unique` 真正执行构造的位置在
标准库模板内部，通常无法访问这个私有构造函数。因此这里使用：

```cpp
std::unique_ptr<Impl>(new Impl())
```

## 从公开 DB 到内部 Impl 的创建链

`DB::Impl::OpenInMemory()` 只创建内部实现对象，还没有创建用户最终拿到的 `DB`。
完整调用关系是：

```text
用户调用 tinylsm::DB::OpenInMemory()
    |
    v
DB::Impl::OpenInMemory()
    |
    +-- new Impl()
    `-- 返回 Result<unique_ptr<Impl>>
    |
    v
new DB(std::move(impl.value()))
    |
    v
DB::impl_ 保存 unique_ptr<Impl>
    |
    v
返回 Result<unique_ptr<DB>>
```

最终所有权关系是：

```text
unique_ptr<DB>
    `-- DB
        `-- impl_: unique_ptr<DB::Impl>
            `-- DB::Impl
                |-- memtable_
                |-- wal_
                |-- table_
                `-- manifest_
```

这就是当前 TinyLSM 的 PImpl 结构：公开类 `DB` 保持接口稳定并隐藏实现细节，
`DB::Impl` 负责协调 MemTable、WAL、SSTable 和 Manifest。

## 当前理解

```text
Impl()                  真正的私有构造函数
DB::Impl::OpenInMemory  调用构造函数的静态工厂函数
new Impl()              在堆上创建内部实现对象
unique_ptr<Impl>        独占并自动管理该对象
Result<unique_ptr<Impl>> 同时表达成功对象或失败状态
```
