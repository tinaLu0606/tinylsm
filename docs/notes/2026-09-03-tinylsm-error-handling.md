# 2026-09-03 · TinyLSM 错误处理设计

## 记录来源

- 类型：技术学习
- 来源：阅读 TinyLSM 的 `Status`、`Result<T>`、DB 实现和 CLI 异常边界，AI 整理
- 状态：`已验证`；内容基于当前源码
- 相关文件：[`../../include/tinylsm/status.h`](../../include/tinylsm/status.h)、[`../../include/tinylsm/result.h`](../../include/tinylsm/result.h)、[`../../include/tinylsm/db.h`](../../include/tinylsm/db.h)、[`../../src/db/db_impl.cpp`](../../src/db/db_impl.cpp)、[`../../tools/tinylsm_cli.cpp`](../../tools/tinylsm_cli.cpp)

## 核心设计

TinyLSM 没有用同一种方式处理所有失败，而是分成两类：

```text
预期内的存储错误
    -> Status / Result<T>

意外的 C++ 异常
    -> 向上传播
    -> CLI main() 最外层 catch
```

预期错误包括 key 不存在、参数错误、文件 IO 失败、磁盘数据损坏、资源耗尽、对象已关闭，以及当前版本不支持的操作。这些情况在存储引擎中并不罕见，调用者通常需要根据错误类别决定下一步，因此使用显式返回值。

意外异常包括 `std::bad_alloc`、标准库或依赖抛出的异常，以及错误访问 `Result`。数据库内部通常无法恢复这些异常，所以不在每一层重复捕获。

## `Status`：只有成功或错误，不返回业务值

`Status` 内部保存：

```text
StatusCode  稳定的错误类别，供程序判断
message     诊断文字，供人阅读
```

适合 `Put`、`Delete`、`Close` 这类不需要返回业务值的操作：

```cpp
Status Put(std::string_view key, std::string_view value);
Status Delete(std::string_view key);
Status Close();
```

调用者首先检查 `ok()`：

```cpp
auto status = db->Put("name", "Tina");
if (!status.ok())
  std::cerr << status.ToString() << '\n';
```

代码应根据 `StatusCode` 分支，不应解析 message：

```cpp
if (status.code() == StatusCode::kNotFound) {
  // 处理不存在
}
```

`WithContext()` 在不改变错误类别的情况下增加调用层信息：

```text
底层：IOError: fsync 000001.wal failed
上层：IOError: close database: fsync 000001.wal failed
```

这样既能让程序继续判断 `kIOError`，也能知道错误发生在哪个操作中。

## `Result<T>`：成功值或错误二选一

`Get` 和 `Open` 成功时需要返回对象，因此不能只返回 `Status`：

```cpp
Result<std::string> Get(std::string_view key);
Result<std::unique_ptr<DB>> Open(...);
```

`Result<T>` 保证两种状态只能存在一种：

```text
成功：value 存在，Status 为 OK
失败：value 不存在，Status 为非 OK
```

正常使用方式：

```cpp
auto value = db->Get("name");
if (!value.ok())
  return value.status();

std::cout << value.value();
```

如果失败后仍调用 `value()`，它会抛出 `BadResultAccess`。这是错误使用 API 的保护，不是正常控制流程。`operator*` 和 `operator->` 也要求调用者已经检查 `ok()`。

这个设计相当于 C++20 下自行实现的简化版：

```cpp
std::expected<T, Status> // C++23
```

## 错误如何沿调用链传播

以打开数据库时读取 Manifest 失败为例：

```text
PosixFileSystem::OpenRandomAccess
    -> 返回 Status::IOError

ManifestState::Load
    -> 返回同一个错误

DB::Impl::LoadManifest
    -> WithContext("load MANIFEST")

DB::Open
    -> 返回失败的 Result<unique_ptr<DB>>

调用者
    -> 检查 opened.ok()
```

典型代码模式是：

```cpp
auto result = DoSomething();
if (!result.ok())
  return result.status().WithContext("outer operation");
```

中间层不需要 `try/catch`，因为它只是把可预期错误继续返回，并补充上下文。

## 为什么不能把错误简单理解成“什么都没发生”

存储操作由多个有副作用的阶段组成：

```text
分配 sequence
    -> 追加 WAL
    -> fsync WAL
    -> 更新 MemTable
    -> 可能 Flush
```

后面的阶段失败时，前面的阶段可能已经完成。例如 WAL 已经写入，但 `fsync` 返回错误；当前调用没有成功确认，重启后却仍可能恢复这条记录。因此公共 API 明确说明：失败的 `Put` 不保证 key 一定保持原样。

Flush 发布 Manifest 时还有三种状态：

```text
NotPublished
    旧 Manifest 仍是权威状态

VisibleNotDurable
    新 Manifest 可能已经可见，但目录同步失败
    当前 DB 进入 terminal state，必须关闭并重新打开

Durable
    新 Manifest 已成为持久化提交状态
```

`terminal_error_` 的作用是阻止状态不确定的 handle 继续读写，避免内存状态和磁盘状态进一步分裂。

## 异常在哪里处理

TinyLSM 库允许意外异常穿过公共接口，例如 `std::bad_alloc`。库内部不在每个函数中捕获，因为中间层通常不知道应该如何安全恢复。

CLI 是进程边界，因此在 `main()` 中统一捕获：

```cpp
try {
  return RunCli(...);
} catch (const BadResultAccess& error) {
  // Result 使用错误
} catch (const std::bad_alloc&) {
  // 内存不足
} catch (const std::exception& error) {
  // 其他标准异常
} catch (...) {
  // 未知异常
}
```

它的目的不是恢复数据库操作，而是把异常转换成清晰的错误信息和非零退出码，避免进程无说明地终止。

## RAII 与显式 `Close()`

RAII 保证 DB 或文件对象析构时尽量释放资源，但析构函数不能方便地把关闭错误返回给调用者。因此：

```text
只要求释放资源
    -> 可以依赖析构

需要知道 Sync/Close 是否成功
    -> 显式调用 DB::Close()
```

这也是 `Close()` 返回 `Status` 的原因。

## 调用者应遵守的规则

1. 每个 `Status` 和 `Result<T>` 都先检查 `ok()`。
2. 使用 `StatusCode` 做程序分支，不解析 message。
3. 只有检查成功后才能访问 `Result::value()`、`operator*` 或 `operator->`。
4. 需要确认关闭错误时显式调用 `Close()`。
5. `Put/Delete` 返回错误时，不假设操作一定完全没有生效。
6. DB 进入 terminal state 后停止数据操作，关闭并重新打开。

## 一句话总结

```text
Status / Result<T> 负责可预期、可判断的存储错误；
异常负责无法在当前层安全处理的意外失败；
CLI 的 try/catch 是最终进程边界。
```
