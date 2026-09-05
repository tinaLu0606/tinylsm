# TinyLSM Lab 设计与实现计划

## 状态

- 最近更新：2026-09-05
- 状态：`Goal 1 已完成；Goal 2–4 待实现`
- 定位：本地、单用户的 TinyLSM 测试与可观测实验台
- 代码边界：前端位于 `tools/lab_web` 子模块；C++ 后端、引擎诊断接口和测试支持位于 TinyLSM 主仓库

## 前置条件

TinyLSM V3 已完成并提交，当前代码已经包含多表读取、完整 Scan、同步 full compaction
和 obsolete-file cleanup。实现 Lab 时仍须以每个 Goal 开始时的实时源码为准，并重新检查
主仓库和前端子模块的 Git 状态，保留无关工作区修改。

## 产品目标

TinyLSM Lab 不是普通 CRUD 管理页，而是用于回答以下问题的工程工具：

1. 一次 Put/Delete/Get/Scan/Compact 实际改变了哪些状态？
2. MemTable、WAL、SSTable 和 Manifest 当前是什么状态？
3. 数据库和 Lab Server 占用了多少内存、CPU 和磁盘？
4. 在可重现 workload 中，结果、顺序、延迟和恢复是否正确？
5. 截断、损坏或未正常关闭后，TinyLSM 会如何恢复或报错？

## 非目标

- 不把 Lab 诊断能力扩张为 TinyLSM 的稳定公开 API。
- 不在前端中实现或复制 LSM 解析逻辑。
- 不把 HTTP 耗时冒充为存储引擎内部耗时。
- 不支持远程多用户、身份验证、云部署或任意目录的破坏性操作。
- Lab 不代替 GoogleTest、恢复测试和 ASan/UBSan。

## 总体架构

```text
Browser / React
    |
    | HTTP + JSON: 命令、查询、分页数据
    | SSE: 事件、workload 进度、聚合指标
    v
tinylsm_lab_server
    |
    +-- Session Controller  ---- 串行化同一 DB 的操作
    +-- Diagnostic Service  ---- 引擎状态快照
    +-- Storage Inspector   ---- Manifest/WAL/SSTable 解码与分页
    +-- Metrics + Log       ---- 聚合指标与 JSONL 操作日志
    +-- Workload Runner     ---- 确定性操作与参考模型
    `-- Recovery Runner     ---- 受控子进程与沙箱故障注入
             |
             v
          TinyLSM
```

开发时由 Vite 把 `/api` 代理到 `127.0.0.1:8080`。打包时由 Lab Server 同时提供前端
`dist/` 和 `/api`，从而只需要一个本地端口。服务器默认只监听 `127.0.0.1`。

## 前端信息架构

### 全局 Session Bar

- 当前实验目录、连接状态、Open、Close、Reopen。
- 展示并编辑 `Options`：MemTable 阈值、SSTable block 目标、key/value 上限和
  `sync_on_write`。
- 始终显示数据来源：`Mock`、`Live`、`Unavailable`。
- 一个 Session 同时只允许一条写路径；workload 运行时禁用直接写操作或排队。

### Playground

- Put、Get、Delete、Scan、Compact；未实现功能由 feature flag 禁用，不伪造成功。
- Text、Hex 和 Base64 三种 key/value 输入与显示方式。
- 批量操作编辑器、单步执行、全部执行、停止和重放。
- 展示 `StatusCode`、诊断 message、耗时和操作前后状态 diff。
- 缺失 key、空 value 和 tombstone 在界面上明确区分。

### Storage Explorer

- 目录树：Manifest、active/obsolete/orphan WAL、live/obsolete/orphan SSTable 和临时文件。
- Manifest：format version、active WAL、next file number、last sequence 和 table metadata。
- WAL：sequence、value/tombstone、key/value 预览、record 长度、offset 和 CRC 结果。
- SSTable：file properties、block 列表、offset/size、key/sequence 范围、entry 和 CRC 结果。
- 原始 Hex 只按范围读取；记录和 block 按页读取，禁止一次把大文件载入浏览器。
- 可从 Manifest table metadata 跳转到对应 SSTable，并标识引用关系和异常文件。

### Timeline & Resources

- 一般操作保留详细事件；workload 只传输 200–500 ms 粒度的聚合数据。
- 事件可按 operation id、阶段、文件和成功/失败过滤。
- 展示 Put/Delete 中的 WAL append/sync、MemTable apply、Flush、SSTable publish、
  Manifest publish、WAL switch 和 Compaction 阶段。
- 资源指标：
  - Lab Server 进程 RSS 和 CPU，明确标注其包含 HTTP Server 开销；
  - TinyLSM `ApproximateMemoryUsage()`，明确标注为 MemTable owned-entry 估算；
  - DB 目录总字节数以及 Manifest/WAL/SSTable/临时文件分项占用；
  - 操作次数、错误数、吞吐量、P50/P95/P99、Flush/Compaction 次数和耗时。
- 图表默认仅保留最近 10 分钟或固定点数，防止内存无限增长。

### Workload

- 可配置 operation count、seed、key space、value size、Put/Get/Delete 比例、操作速率、
  顺序/均匀随机/热点分布和定期 Reopen。
- 支持 Start、Pause、Resume 和 Cancel。
- 后端使用确定性随机数生成器和参考有序 map 模型；前端不自行判定 TinyLSM 结果。
- 结果保留 seed、Options、操作统计、延迟聚合、文件增长和首个正确性差异。

### Recovery Lab

第一版只实现三个场景：

1. 子进程在不调用 `Close()` 的情况下终止，然后重开；
2. 复制实验并截断 active WAL 尾部，然后重开；
3. 复制实验并修改 WAL/SSTable/Manifest 的一个受控字节，验证 CRC/损坏错误。

前端必须先显示将要改动的沙箱副本、预期结果和恢复方式。后端只允许修改 Lab
创建且经 canonical-path 验证的 session 目录；绝不允许对用户任意输入路径执行截断、删除
或字节损坏。

### Operation Log & Report

- 后端为权威日志，以 bounded ring buffer 提供最近事件，并可选写入按大小轮转的 session JSONL。
- 前端 IndexedDB 保留最近实验、UI 筛选和展开状态，但不冒充后端权威记录。
- 默认最多保留 10,000 条详细日志；大 value 仅保存长度、编码和截断预览。
- performance mode 禁用逐条持久化，仅保留聚合指标和首个失败样本。
- 支持导出/导入实验 JSON、复制复现步骤和生成 bug report。

## 前后端合同

### 通信方式

- HTTP/JSON：Session 管理、数据操作、状态快照、分页文件数据和实验控制。
- SSE：引擎事件、资源采样、workload 进度和 recovery 进度。第一版不使用 WebSocket。
- 所有二进制 key/value 在 JSON 中使用 Base64，并同时附带原始字节长度。
- 错误统一返回 `StatusCode`、message、operation id 和可选阶段，前端不依赖 message 分支。

### 核心路由

```text
POST /api/session/open
POST /api/session/close
POST /api/session/reopen
GET  /api/session/state

POST /api/operations/put
POST /api/operations/get
POST /api/operations/delete
POST /api/operations/scan
POST /api/operations/compact

GET  /api/storage/files
GET  /api/storage/manifest
GET  /api/storage/wal/{name}/records?cursor=&limit=
GET  /api/storage/sst/{name}/blocks?cursor=&limit=
GET  /api/storage/file/{name}/bytes?offset=&length=

GET  /api/metrics
GET  /api/events
POST /api/workloads
POST /api/workloads/{id}/pause
POST /api/workloads/{id}/resume
POST /api/workloads/{id}/cancel
POST /api/recovery/experiments
GET  /api/recovery/experiments/{id}
```

### 诊断边界

引擎侧增加非公开的 `DiagnosticSnapshot`、`MetricsSnapshot` 和可选事件 sink。诊断钩子必须：

- 默认关闭或零成本接近；
- 不改变 WAL-first、Manifest commit point 和错误语义；
- 不允许回调异常改变 DB 操作结果；
- 只读快照不暴露内部所有权或可变引用；
- 后端通过单一 session executor 调用非线程安全的 DB。

## 性能与安全约束

- 文件、WAL record、SSTable block、entry 和日志全部分页；服务器限制单次响应字节数。
- SSE 批量发送 workload 进度；不为每个高频操作触发一次 React 全树刷新。
- 前端日志使用虚拟列表，图表使用有界 ring buffer，大数据解码留在后端。
- 资源采样默认 1 Hz；分位数由后端计算，不把全部 latency sample 传到浏览器。
- Lab Server 仅监听 loopback，限制 request body，严格解析数字边界并拒绝路径穿越。
- 故障注入使用沙箱副本和受控 worker 进程，不向普通 operation API 暴露文件删除/损坏能力。

## Goal 分块与实现顺序

整体包含两个代码面：前端子模块与 C++ 后端。测试、worker 和故障注入是支撑这两个
代码面的验收基础，不单独作为第三个产品。

### Goal 1：完整前端与 Mock 合同（已完成）

**目标**：一次完成全部页面、交互、响应式布局和 typed API contract，使用确定性 `MockLabApi`
演示真实状态迁移，但所有模拟数据都明确标注。

**交付物**：

- feature-based React/TypeScript 目录、`LabApi`、contracts、`MockLabApi` 和 fixtures；
- Session Bar、Playground、Storage、Timeline & Resources、Workload、Recovery 和 Report；
- loading/empty/disconnected/partial/error 状态、键盘导航、桌面/移动布局；
- 确定性 workload/recovery 模拟和 IndexedDB 前端保留；
- 前端架构说明、API contract 说明和未接入后端的限制；
- 前端单元/组件测试和浏览器冒烟验证。

**验收**：

```text
npm ci
npm test -- --run
npm run build
启动 Vite，在桌面和移动宽度遍历全部页面
无 console error，主要交互可操作，Mock/Unavailable 标识明确
```

已于 2026-09-05 在 `tools/lab_web` 完成：typed contract、确定性
`MockLabApi`、六个工作区、IndexedDB 保留、窗口化日志和前端测试。验收记录为
`npm ci`、`npm test -- --run`（7 tests）和 `npm run build` 通过，并已在桌面和
390 px 移动宽度遍历页面且无 console error。C++ 后端、真实指标和真实故障注入仍不在
Goal 1 范围内。

**Goal prompt**：

```text
/goal Implement Goal 1 from docs/plans/tinylsm-lab-design.md. Complete the entire
frontend and deterministic MockLabApi without stopping until the frontend tests,
production build, and desktop/mobile browser inspection pass. Preserve unrelated
changes, do not implement the C++ backend, and do not commit or push.
```

### Goal 2：C++ Lab Server、诊断快照与真实基础操作

**前置**：Goal 1 已验收，并重新确认 TinyLSM V3 的实际接口与测试状态。

**目标**：实现 `tinylsm_lab_server`、非公开诊断快照、Session executor、基础 HTTP/SSE 合同和
`HttpLabApi`，让 Open/Close/Reopen/Put/Get/Delete/Scan/Compact、实时状态、文件列表和操作日志
使用真实 TinyLSM。

**工程要求**：

- 后端直接链接 `tinylsm`，禁止 shell out 到 CLI；
- 选择轻量 HTTP/JSON 依赖时固定确切版本并记录理由；
- 诊断信息使用内部 peer/service，不扩大 `include/tinylsm/db.h` 公开承诺；
- 同一 session 的命令全部串行执行；
- SSE 断线可重连，事件 buffer 有上限；
- 后端异常在 HTTP 边界转为可诊断 5xx，但不吞掉 TinyLSM `Status`。

**验收**：

```text
./run test
新增 Lab Server contract/integration tests
npm test -- --run
npm run build
从浏览器完成 Open -> Put -> Get -> Scan -> Close -> Reopen
界面与后端 state/log 一致，无 Mock 数据混入 Live session
```

**Goal prompt**：

```text
/goal Implement Goal 2 from docs/plans/tinylsm-lab-design.md. Build the C++ Lab
Server, diagnostic snapshot, serialized live session API, SSE events, and HttpLabApi
integration. Continue until C++ tests, frontend tests/build, and the live browser
operation flow pass. Preserve unrelated changes and do not commit or push.
```

### Goal 3：深度 Storage Inspector、资源指标与 Workload

**目标**：完成文件级观察、有界操作日志、真实资源指标、聚合时间线和可重现 workload，
并通过参考有序 map 模型检查正确性。

**交付物**：

- Manifest/WAL/SSTable 分页解码 API、range Hex API 和 CRC/结构错误展示；
- Lab Server RSS/CPU、DB 目录分项占用和 MemTable 估算指标；
- 操作延迟、吞吐、错误、Flush/Compaction 指标与按大小轮转的 JSONL 日志；
- 确定性 workload runner、pause/resume/cancel、参考模型和失败复现数据；
- 前端虚拟列表、分页、有界图表和实验报告导出。

**验收**：

```text
./run test
./run asan
npm test -- --run
npm run build
用固定 seed 运行 workload 两次，生成相同的操作与正确性结果
大日志和大文件场景下界面不一次加载全部数据
性能模式不写逐条持久化日志
```

**Goal prompt**：

```text
/goal Implement Goal 3 from docs/plans/tinylsm-lab-design.md. Complete the paged
storage inspector, bounded logging and metrics, deterministic workload runner,
reference-model validation, and real frontend views. Continue until all C++ and
frontend tests, sanitizers, build, and reproducibility checks pass. Do not commit
or push.
```

### Goal 4：Recovery Lab、打包与端到端收口

**目标**：以独立实验 worker、沙箱副本和可复用故障注入支持实现三个 Recovery 场景，
然后完成单命令启动、前端静态文件服务、端到端测试和文档收口。

**工程要求**：

- 把测试所需的 fault-injection 概念整理为一个非公开 support target，供 GoogleTest 和 Lab 共用，
  但不链入正常 `tinylsm` 库；
- 真实未 Close 终止必须使用 worker 子进程，不粗暴终止 Lab Server；
- 截断和损坏只能对服务器创建的沙箱副本执行；
- 所有破坏动作必须有 preview、审计日志、边界验证和 reset；
- 新增 `./run lab` 或等价命令，最终由一个本地服务同时提供 UI 和 API。

**验收**：

```text
./run test
./run asan
npm test -- --run
npm run build
三个 Recovery 场景都有自动化合同/恢复测试
路径穿越、非沙箱目录和超限请求被拒绝
从干净 clone + submodule init 能按 README 启动 Lab
浏览器端到端完成 CRUD、查看状态、workload 和一个 recovery 场景
```

**Goal prompt**：

```text
/goal Implement Goal 4 from docs/plans/tinylsm-lab-design.md. Build the sandboxed
Recovery Lab worker and three recovery scenarios, then finish single-command local
startup, static UI serving, end-to-end tests, README, and security validation.
Continue until every listed acceptance check passes. Preserve unrelated changes
and do not commit or push.
```

## Git 与子模块交付顺序

每个涉及两个仓库的 Goal 在用户审查后按以下顺序提交：

```text
1. 在 tinylsm-lab-ui 仓库提交前端修改
2. 回到 tinylsm 主仓库更新 tools/lab_web gitlink
3. 提交 C++ 后端、测试、README 和子模块指针
4. 分别检查两个仓库的 status/log；不自动 push
```

任何 Goal 都不得把用户已有的无关工作区修改带入提交。

## 最终完成条件

TinyLSM Lab 只在以下条件全部满足时视为完成：

- 页面覆盖 Playground、Storage、Timeline & Resources、Workload 和 Recovery；
- Mock 与 Live 数据严格区分，所有 Live 页面由 `HttpLabApi` 驱动；
- 数据操作、文件观察、资源指标、日志、workload 和三个 recovery 场景可实际运行；
- 对大日志、大文件和高频 workload 实施了分页、采样、聚合和有界保留；
- C++ 测试、前端测试、ASan/UBSan、前端构建和端到端浏览器验证通过；
- 根 README 和前端 README 准确说明架构、运行方式、指标语义、沙箱限制和已知边界。
