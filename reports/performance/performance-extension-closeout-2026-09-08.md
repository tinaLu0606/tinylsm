# TinyLSM Performance Extension Closeout

状态：`Goal 1/2/3 与当前 revision Linux Release CLI smoke 已完成`

本文件收束 `performance-extension-roadmap.md` 的 Goal 1-3。它是证据索引，不是将
不同 workload 的吞吐、RSS 或 p99 汇成一个分数的综合 benchmark。

## 已完成的三项扩展

| Goal | 决策与实现 | 固定 Linux 证据 | 主要边界 |
| --- | --- | --- | --- |
| 1 · 读取 | `perf cpu-clock` 指向重复 decode/CRC 后，实现有界、仅缓存验证后 block 的 LRU Block Cache；Get/Scan 使用 shared lock | 11-case before/after JSON 各两轮，目标 CV 均低于 10%；Debug/ASan/TSan `101/101`，Release | Scan materialization 持 shared lock，仍可能延迟 writer；无 Bloom Filter |
| 2 · 写入 | 一个 immutable MemTable/WAL 与单 worker background flush，Manifest v3 记录 active/immutable WAL | async `123,050 / 127,375 ops/s`，sync `4,775 / 4,773 ops/s`；两轮 CV 低于 10%；Debug/ASan/TSan `102/102`，Release | 一个 immutable generation；无 Group Commit |
| 3 · Compaction | 默认合并最老连续四表的 simplified size-tiered；后台 job 使用 shared reader snapshot 和 Manifest file-number reservation | size-tiered `19,266 / 19,640 ops/s`，Manual `10,358 / 10,475 ops/s`；两轮 CV 低于 10%；Debug/ASan/TSan `107/107`，Release | 无 leveled layout；前台计时不含 Manual 后置 full compaction 或 Close drain |

Goal 1 的代表性 single-SSTable hit 从约 `10.1k` 提高到 `1.36M ops/s`；Goal 3 将 mixed
workload point-read amplification 从约 `16.4` 降到 `2.14`，但 write amplification 从
`1.286` 增到 `7.120`。这些数字只在各自报告定义的 workload、环境与计时边界内有效。

## 原始证据

- Goal 1：[读取报告](read-path-2026-09-07.md)；两轮 before/after JSON 位于
  `results/goal1-read-*-linux-2026-09-07-run*.json`。
- Goal 2：[异步写入报告](write-path-2026-09-07.md)；两轮 JSON 位于
  `results/goal2-write-linux-2026-09-07-run*.json`。
- Goal 3：[Compaction 报告](compaction-2026-09-08.md)；两轮 JSON 位于
  `results/goal3-compaction-linux-2026-09-08-run*.json`。
- 固定 VM 定义：[`tools/vm/tinylsm-linux.yaml`](../../tools/vm/tinylsm-linux.yaml)

每份报告都保留各自的 benchmark method、环境、median、CV、原始计数和适用边界。RSS
只在 Goal 1/2 的特定 wrapper/workload 下采集，不能与 Goal 3 或其他 workload 比较。

## 有意识的延期项

- Bloom Filter：Goal 1 的 cache 后 negative lookup 已不再被重复 decode/CRC 主导；没有
  证据支持增加新 per-table metadata format。
- Group Commit：Goal 2 没有 multiwriter queue contention profile，因而当时不能证明
  batching 值得引入新的 queue、sequence 和 durable-error semantics。后续 Goal 6 已实现
  其功能语义，但正式吞吐/尾延迟结论仍待单独 multiwriter 实验。
- Leveled compaction：Goal 3 的 size-tiered 已以可解释的 write-amplification 代价降低
  table pressure 和前台读尾延迟；没有已测 leveled A/B，不能声称它更优。

这些不是未完成 bug。重新考虑它们的前提是新的目标 workload 或 profile 显示当前边界
成为主要成本。

## Linux Release deployment smoke

固定 Linux VM 从本地 Git bundle clone 了干净的 `746741e` checkout：环境文件记录了
空的 `git status --short`、bundle SHA-256、Ubuntu ARM64/ext4 和工具版本。Release build
后，独立 CLI 进程依次完成：

```text
put deployment linux-ok
reopen + get deployment              -> linux-ok
reopen + get deployment --json       -> Base64 JSON
reopen + scan                        -> deployment<TAB>linux-ok
reopen + scan --json                 -> Base64 JSON
```

验证日志对文本值、tab-separated Scan 行和 JSON 行都做了精确断言。证据文件为：

- `results/goal3-release-smoke-linux-2026-09-08-environment.txt`
- `results/goal3-release-smoke-linux-2026-09-08-release-build.log`
- `results/goal3-release-smoke-linux-2026-09-08-cli-smoke.log`
- `results/goal3-release-smoke-linux-2026-09-08-verification.log`

`tools/lab_web` 在该 clean checkout 中未初始化；Release CLI 不依赖它，因此此项验证不
代表 Lab UI 的部署验收。Lab 需要时仍应以其 submodule 为单位单独构建和验证。

## 后续触发条件

不自动创建 Goal 4。下一项工作必须先定义真实 workload 和可复核门槛，例如：多 writer
同步写入的 queue profile、cache 后仍昂贵的 negative lookup，或数据规模增大后的
end-to-end compaction write cost（包含 Close/后置 maintenance）。
