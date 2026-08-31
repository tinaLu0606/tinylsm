# 2026-08-27 · C++ 头文件与实现文件的组织方式

## 记录来源

- 类型：技术学习
- 来源：阅读 `src/sstable/sstable_format.h` 时产生的问题，AI 整理
- 状态：`已验证`；结论基于当前 TinyLSM 源码
- 相关文件：[`../../src/sstable/sstable_format.h`](../../src/sstable/sstable_format.h)、[`../../src/sstable/sstable_builder.cpp`](../../src/sstable/sstable_builder.cpp)、[`../../src/sstable/sstable_reader.cpp`](../../src/sstable/sstable_reader.cpp)

## 核心结论

C++ 不要求每个头文件都有同名 `.cpp`。头文件主要用于声明多个翻译单元共同依赖的接口、类型和常量；函数定义可以位于任意合适的 `.cpp` 中，只要最终参与同一个目标的编译和链接。

常见组织方式有三种：

1. `foo.h + foo.cpp`：头文件声明，源文件定义。这是常见约定，但不是语言要求。
2. 只有头文件：模板、短小 `inline` 函数、`inline constexpr` 常量等直接定义在头文件中，属于 header-only 形式。
3. 一个公共头文件由其他命名的 `.cpp` 实现：多个模块共享声明，实现按功能集中在某个源文件中。

## `sstable_format.h` 在当前项目中的作用

该文件定义或声明了 SSTable 磁盘格式的共同契约：

- `kSstableMagic`、`kSstableVersion`、`kSstableFooterSize`：`inline constexpr` 格式常量；
- `BlockMeta`、`Footer`：Builder 和 Reader 都需要知道完整布局的结构体；
- `EncodeDataBlock`、`DecodeDataBlock`、`EncodeIndex`、`DecodeIndex`、`EncodeFooter`、`DecodeFooter`：编解码函数声明。

这些编解码函数的定义当前位于 `sstable_builder.cpp`。Builder 调用 Encode 函数，Reader 调用 Decode 函数：

```text
sstable_format.h
  |-- 格式常量与结构体
  `-- Encode/Decode 函数声明
          |                    |
          v                    v
  sstable_builder.cpp   sstable_reader.cpp
  定义并调用 Encode     调用 Decode
```

链接阶段会把 `sstable_builder.cpp` 中编译出的函数定义提供给 `sstable_reader.cpp`，因此不需要存在同名的 `sstable_format.cpp`。

## 这是不是 header-only

不是。判断标准不是“有没有同名 `.cpp`”，而是函数实现是否位于头文件中。

当前情况是：

```text
常量和结构体定义：sstable_format.h
编解码函数声明：  sstable_format.h
编解码函数定义：  sstable_builder.cpp
```

真正的 header-only 通常会把函数定义也放在头文件中，并使用模板或 `inline` 避免多个翻译单元产生重复定义。

## 当前组织是否合理

功能上正确，也符合 C++ 编译和链接规则。`sstable_format.h` 作为 Builder 与 Reader 的共享格式契约很合理。

命名上存在一个小改进空间：`sstable_builder.cpp` 同时承担 Builder 实现和通用格式编解码实现。项目扩大后，可将 `Encode/Decode` 定义移动到 `sstable_format.cpp`，形成：

```text
sstable_format.h/.cpp  磁盘格式与编解码
sstable_builder.h/.cpp SSTable 构建流程
sstable_reader.h/.cpp  SSTable 读取流程
```

这属于职责清晰度优化，不是当前代码的正确性问题。

## 编译层面的理解

每个 `.cpp` 会独立编译成目标文件：

```text
sstable_builder.cpp -> sstable_builder.o
sstable_reader.cpp  -> sstable_reader.o
```

头文件通过 `#include` 把声明提供给各个 `.cpp`。最终链接器把调用和唯一函数定义连接起来：

```text
sstable_reader.o 中的 DecodeDataBlock 调用
                    |
                    v
sstable_builder.o 中的 DecodeDataBlock 定义
```

如果只有声明却没有任何函数定义，编译可能通过，但最终链接会报 `undefined reference` 或 macOS 上的 `Undefined symbols`。
