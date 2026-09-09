#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "tinylsm/compaction_metrics.h"
#include "tinylsm/db.h"
#include "tinylsm/read_metrics.h"
#include "tinylsm/write_batch.h"
#include "tinylsm/write_metrics.h"

#include "sstable/sstable_format.h"
#include "util/coding.h"

#ifdef TINYLSM_HAVE_LEVELDB
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kDefaultMemtableBytes = 4U * 1024U * 1024U;
constexpr std::size_t kCompactionMemtableBytes = 512U * 1024U;
constexpr std::size_t kGroupCommitMemtableBytes = 64U * 1024U * 1024U;
constexpr std::uint64_t kSnapshotSetupBatchOperations = 500'000;
constexpr std::size_t kSnapshotSetupQueueBytes = 128U * 1024U * 1024U;

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label) {
    static std::uint64_t next = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("tinylsm-bench-" + std::string(label) + "-" +
             std::to_string(Clock::now().time_since_epoch().count()) + "-" +
             std::to_string(next++));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

class Engine {
public:
  virtual ~Engine() = default;
  virtual bool Put(std::string_view key, std::string_view value) = 0;
  virtual bool
  WriteBatch(const std::vector<std::pair<std::string, std::string>>& entries) = 0;
  virtual bool Get(std::string_view key, bool expect_found) = 0;
  virtual bool Scan(std::string_view begin, std::size_t expected) = 0;
  virtual bool Compact() = 0;
  virtual bool Close() = 0;
  [[nodiscard]] virtual tinylsm::ReadMetrics ReadMetricsSnapshot() const = 0;
};

class TinyLsmEngine final : public Engine {
public:
  static std::unique_ptr<TinyLsmEngine> Open(const std::filesystem::path& path,
                                             bool sync, std::size_t memtable_bytes,
                                             std::size_t block_cache_bytes) {
    tinylsm::Options options;
    options.sync_on_write = sync;
    options.memtable_bytes = memtable_bytes;
    options.block_cache_bytes = block_cache_bytes;
    auto opened = tinylsm::DB::Open(path, options);
    if (!opened.ok())
      return nullptr;
    return std::unique_ptr<TinyLsmEngine>(new TinyLsmEngine(std::move(opened.value())));
  }

  bool Put(std::string_view key, std::string_view value) override {
    return db_->Put(key, value).ok();
  }

  bool
  WriteBatch(const std::vector<std::pair<std::string, std::string>>& entries) override {
    tinylsm::WriteBatch batch;
    for (const auto& [key, value] : entries)
      batch.Put(key, value);
    return db_->Write(batch).ok();
  }

  bool Get(std::string_view key, bool expect_found) override {
    auto result = db_->Get(key);
    return expect_found ? result.ok()
                        : result.status().code() == tinylsm::StatusCode::kNotFound;
  }

  bool Scan(std::string_view begin, std::size_t expected) override {
    auto result = db_->Scan(begin, {});
    return result.ok() && result.value().size() == expected;
  }

  bool Compact() override { return db_->Compact().ok(); }
  bool Close() override { return db_->Close().ok(); }
  [[nodiscard]] tinylsm::ReadMetrics ReadMetricsSnapshot() const override {
    return db_->GetReadMetrics();
  }

private:
  explicit TinyLsmEngine(std::unique_ptr<tinylsm::DB> db) : db_(std::move(db)) {}
  std::unique_ptr<tinylsm::DB> db_;
};

#ifdef TINYLSM_HAVE_LEVELDB
class LevelDbEngine final : public Engine {
public:
  static std::unique_ptr<LevelDbEngine> Open(const std::filesystem::path& path,
                                             bool sync, std::size_t, std::size_t) {
    leveldb::Options options;
    options.create_if_missing = true;
    options.compression = leveldb::kNoCompression;
    leveldb::DB* raw = nullptr;
    auto status = leveldb::DB::Open(options, path.string(), &raw);
    if (!status.ok())
      return nullptr;
    return std::unique_ptr<LevelDbEngine>(new LevelDbEngine(raw, sync));
  }

  bool Put(std::string_view key, std::string_view value) override {
    return db_
        ->Put(write_options_, leveldb::Slice(key.data(), key.size()),
              leveldb::Slice(value.data(), value.size()))
        .ok();
  }

  bool
  WriteBatch(const std::vector<std::pair<std::string, std::string>>& entries) override {
    leveldb::WriteBatch batch;
    for (const auto& [key, value] : entries)
      batch.Put(key, value);
    return db_->Write(write_options_, &batch).ok();
  }

  bool Get(std::string_view key, bool expect_found) override {
    std::string value;
    auto status = db_->Get(leveldb::ReadOptions(),
                           leveldb::Slice(key.data(), key.size()), &value);
    return expect_found ? status.ok() : status.IsNotFound();
  }

  bool Scan(std::string_view begin, std::size_t expected) override {
    std::unique_ptr<leveldb::Iterator> iterator(
        db_->NewIterator(leveldb::ReadOptions()));
    iterator->Seek(leveldb::Slice(begin.data(), begin.size()));
    std::size_t count = 0;
    while (iterator->Valid()) {
      ++count;
      iterator->Next();
    }
    return iterator->status().ok() && count == expected;
  }

  bool Compact() override { return false; }

  bool Close() override {
    db_.reset();
    return true;
  }
  [[nodiscard]] tinylsm::ReadMetrics ReadMetricsSnapshot() const override { return {}; }

private:
  LevelDbEngine(leveldb::DB* db, bool sync) : db_(db) { write_options_.sync = sync; }

  std::unique_ptr<leveldb::DB> db_;
  leveldb::WriteOptions write_options_;
};
#endif

enum class EngineKind { kTinyLsm, kLevelDb };

std::unique_ptr<Engine> OpenEngine(EngineKind kind, const std::filesystem::path& path,
                                   bool sync, std::size_t memtable_bytes,
                                   std::size_t block_cache_bytes = 8U * 1024U * 1024U) {
  if (kind == EngineKind::kTinyLsm)
    return TinyLsmEngine::Open(path, sync, memtable_bytes, block_cache_bytes);
#ifdef TINYLSM_HAVE_LEVELDB
  return LevelDbEngine::Open(path, sync, memtable_bytes, block_cache_bytes);
#else
  (void)path;
  (void)sync;
  (void)memtable_bytes;
  (void)block_cache_bytes;
  return nullptr;
#endif
}

std::string Key(std::uint64_t index) {
  std::ostringstream out;
  out << 'k' << std::setw(15) << std::setfill('0') << index;
  return out.str();
}

std::string Value(std::uint64_t index, std::size_t bytes) {
  std::string value = "v" + std::to_string(index);
  value.resize(bytes, static_cast<char>('a' + index % 26));
  return value;
}

std::uint64_t NextRandom(std::uint64_t& state) {
  state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  return state;
}

void SetMeasurements(benchmark::State& state, std::uint64_t operations,
                     std::uint64_t bytes, double elapsed) {
  state.SetIterationTime(elapsed);
  state.SetItemsProcessed(static_cast<std::int64_t>(operations));
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
  state.counters["operations"] = static_cast<double>(operations);
  state.counters["ops_per_second"] =
      benchmark::Counter(static_cast<double>(operations), benchmark::Counter::kIsRate);
}

void SetReadMeasurements(benchmark::State& state, std::uint64_t operations,
                         std::uint64_t bytes, double elapsed,
                         const tinylsm::ReadMetrics& before,
                         const tinylsm::ReadMetrics& after) {
  SetMeasurements(state, operations, bytes, elapsed);
  state.counters["table_probes"] = after.table_probes - before.table_probes;
  state.counters["block_reads"] = after.block_reads - before.block_reads;
  state.counters["block_decodes"] = after.block_decodes - before.block_decodes;
  state.counters["cache_hits"] = after.cache_hits - before.cache_hits;
  state.counters["cache_misses"] = after.cache_misses - before.cache_misses;
  state.counters["cache_inserts"] = after.cache_inserts - before.cache_inserts;
  state.counters["cache_evictions"] = after.cache_evictions - before.cache_evictions;
  state.counters["cache_charge_bytes"] = after.cache_charge_bytes;
}

double Percentile(std::vector<double> samples, double percentile) {
  if (samples.empty())
    return 0.0;
  const auto index = static_cast<std::size_t>(
      std::ceil(percentile * static_cast<double>(samples.size() - 1)));
  std::nth_element(samples.begin(), samples.begin() + index, samples.end());
  return samples[index];
}

std::uint64_t ResidentMemoryKilobytes() {
#ifdef __linux__
  std::ifstream status("/proc/self/status");
  std::string label;
  std::uint64_t kilobytes = 0;
  while (status >> label) {
    if (label == "VmRSS:") {
      status >> kilobytes;
      return kilobytes;
    }
    status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
#endif
  return 0;
}

void WriteFlushLatency(benchmark::State& state, bool sync, std::uint64_t count,
                       std::size_t value_bytes, std::size_t memtable_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(sync ? "write-flush-sync" : "write-flush-async");
    tinylsm::Options options;
    options.sync_on_write = sync;
    options.memtable_bytes = memtable_bytes;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError("open failed");
      return;
    }

    auto db = std::move(opened.value());
    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(count));
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto operation_started = Clock::now();
      const auto status = db->Put(Key(i), Value(i, value_bytes));
      if (!status.ok()) {
        state.SkipWithError(status.ToString().c_str());
        return;
      }
      latencies.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - operation_started)
              .count());
    }
    const auto stopped = Clock::now();
    if (!db->Close().ok()) {
      state.SkipWithError("close failed");
      return;
    }
    const auto metrics = db->GetWriteMetrics();

    SetMeasurements(state, count, count * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["put_p50_us"] = Percentile(latencies, 0.50);
    state.counters["put_p95_us"] = Percentile(latencies, 0.95);
    state.counters["put_p99_us"] = Percentile(latencies, 0.99);
    state.counters["wal_syncs"] = static_cast<double>(metrics.wal_syncs);
    state.counters["memtable_rotations"] =
        static_cast<double>(metrics.memtable_rotations);
    state.counters["background_flushes"] =
        static_cast<double>(metrics.background_flushes);
    state.counters["background_queue_depth_max"] =
        static_cast<double>(metrics.max_background_queue_depth);
    state.counters["flush_stall_ns"] =
        static_cast<double>(metrics.backpressure_wait_nanoseconds);
  }
}

void FillSequential(benchmark::State& state, EngineKind kind, bool sync,
                    std::uint64_t count, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("fill");
    auto engine = OpenEngine(kind, directory.path(), sync, kDefaultMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }

    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
      if (!engine->Put(Key(i), Value(i, value_bytes))) {
        state.SkipWithError("put failed");
        return;
      }
    }
    const auto stopped = Clock::now();
    if (!engine->Close()) {
      state.SkipWithError("close failed");
      return;
    }
    SetMeasurements(state, count, count * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void FillBatch(benchmark::State& state, EngineKind kind, std::uint64_t count,
               std::size_t batch_size, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("batch");
    auto engine = OpenEngine(kind, directory.path(), true, kDefaultMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }

    const auto started = Clock::now();
    for (std::uint64_t first = 0; first < count; first += batch_size) {
      std::vector<std::pair<std::string, std::string>> entries;
      const auto current = std::min<std::uint64_t>(batch_size, count - first);
      entries.reserve(static_cast<std::size_t>(current));
      for (std::uint64_t offset = 0; offset < current; ++offset) {
        entries.emplace_back(Key(first + offset), Value(first + offset, value_bytes));
      }
      if (!engine->WriteBatch(entries)) {
        state.SkipWithError("batch write failed");
        return;
      }
    }
    const auto stopped = Clock::now();
    if (!engine->Close()) {
      state.SkipWithError("close failed");
      return;
    }
    SetMeasurements(state, count, count * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void ConcurrentFill(benchmark::State& state, EngineKind kind, std::size_t thread_count,
                    std::uint64_t writes_per_thread, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("concurrent");
    auto engine = OpenEngine(kind, directory.path(), false, kDefaultMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }

    std::vector<int> succeeded(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    const auto started = Clock::now();
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
      threads.emplace_back([&, thread] {
        bool ok = true;
        const auto base = thread * writes_per_thread;
        for (std::uint64_t index = 0; index < writes_per_thread && ok; ++index) {
          ok = engine->Put(Key(base + index), Value(index, value_bytes));
        }
        succeeded[thread] = ok ? 1 : 0;
      });
    }
    for (auto& thread : threads)
      thread.join();
    const auto stopped = Clock::now();
    if (std::find(succeeded.begin(), succeeded.end(), 0) != succeeded.end()) {
      state.SkipWithError("concurrent put failed");
      return;
    }

    const auto operations = writes_per_thread * thread_count;
    SetMeasurements(state, operations, operations * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void ReadRandom(benchmark::State& state, EngineKind kind, bool found,
                std::uint64_t records, std::uint64_t reads, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(found ? "read-hit" : "read-miss");
    auto engine = OpenEngine(kind, directory.path(), false, kDefaultMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }
    for (std::uint64_t i = 0; i < records; ++i) {
      if (!engine->Put(Key(i), Value(i, value_bytes))) {
        state.SkipWithError("prepare put failed");
        return;
      }
    }

    std::uint64_t random = 0x5eedULL;
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
      const auto index = NextRandom(random) % records + (found ? 0 : records);
      if (!engine->Get(Key(index), found)) {
        state.SkipWithError("get result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetMeasurements(state, reads, reads * (16 + (found ? value_bytes : 0)),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

bool PutBatch(Engine& engine, std::uint64_t records, std::size_t value_bytes,
              std::uint64_t first = 0, std::uint64_t stride = 1) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(static_cast<std::size_t>(records));
  for (std::uint64_t i = 0; i < records; ++i) {
    const auto index = first + i * stride;
    entries.emplace_back(Key(index), Value(index, value_bytes));
  }
  return engine.WriteBatch(entries);
}

void ReadMemTableHit(benchmark::State& state, std::uint64_t records,
                     std::uint64_t reads, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("read-memtable");
    auto engine =
        OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 256U * 1024U * 1024U);
    if (!engine || !PutBatch(*engine, records, value_bytes)) {
      state.SkipWithError("prepare MemTable failed");
      return;
    }

    std::uint64_t random = 0x5eedULL;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
      if (!engine->Get(Key(NextRandom(random) % records), true)) {
        state.SkipWithError("MemTable get result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetReadMeasurements(state, reads, reads * (16 + value_bytes),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ReadSingleTableHit(benchmark::State& state, std::uint64_t records,
                        std::uint64_t reads, std::size_t value_bytes,
                        std::size_t block_cache_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("read-single-table");
    auto engine =
        OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1, block_cache_bytes);
    if (!engine || !PutBatch(*engine, records, value_bytes)) {
      state.SkipWithError("prepare single SSTable failed");
      return;
    }

    std::uint64_t random = 0x5eedULL;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
      if (!engine->Get(Key(NextRandom(random) % records), true)) {
        state.SkipWithError("single-SSTable get result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetReadMeasurements(state, reads, reads * (16 + value_bytes),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ReadMultiTable(benchmark::State& state, bool found, std::uint64_t records,
                    std::uint64_t reads, std::size_t value_bytes) {
  constexpr std::uint64_t kTables = 4;
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(found ? "read-multi-hit" : "read-multi-miss");
    auto engine = OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }
    for (std::uint64_t table = 0; table < kTables; ++table) {
      if (!PutBatch(*engine, records / kTables, value_bytes, table * 2, kTables * 2)) {
        state.SkipWithError("prepare overlapping SSTables failed");
        return;
      }
    }

    std::uint64_t random = 0x5eedULL;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
      const auto base = NextRandom(random) % (records / kTables);
      const auto index = found ? base * kTables * 2 : base * kTables * 2 + 1;
      if (!engine->Get(Key(index), found)) {
        state.SkipWithError("multi-SSTable get result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetReadMeasurements(state, reads, reads * (16 + (found ? value_bytes : 0)),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ReadRepeatedHit(benchmark::State& state, std::uint64_t records,
                     std::uint64_t working_set, std::uint64_t reads,
                     std::size_t value_bytes, std::size_t block_cache_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("read-repeated");
    auto engine =
        OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1, block_cache_bytes);
    if (!engine || !PutBatch(*engine, records, value_bytes)) {
      state.SkipWithError("prepare repeated-hit SSTable failed");
      return;
    }
    for (std::uint64_t i = 0; i < working_set; ++i) {
      if (!engine->Get(Key(i), true)) {
        state.SkipWithError("warmup get failed");
        return;
      }
    }

    std::uint64_t random = 0x5eedULL;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
      if (!engine->Get(Key(NextRandom(random) % working_set), true)) {
        state.SkipWithError("repeated get result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetReadMeasurements(state, reads, reads * (16 + value_bytes),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ScanTail(benchmark::State& state, EngineKind kind, std::uint64_t records,
              std::size_t returned, std::size_t scans, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("scan");
    auto engine = OpenEngine(kind, directory.path(), false, kDefaultMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }
    for (std::uint64_t i = 0; i < records; ++i) {
      if (!engine->Put(Key(i), Value(i, value_bytes))) {
        state.SkipWithError("prepare put failed");
        return;
      }
    }

    const auto started = Clock::now();
    bool correct = true;
    for (std::size_t scan = 0; scan < scans && correct; ++scan)
      correct = engine->Scan(Key(records - returned), returned);
    const auto stopped = Clock::now();
    if (!correct) {
      state.SkipWithError("scan result mismatch");
      return;
    }
    const auto operations = returned * scans;
    SetMeasurements(state, operations, operations * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void ScanSingleTable(benchmark::State& state, std::uint64_t records,
                     std::size_t returned, std::size_t scans, std::size_t value_bytes,
                     std::size_t block_cache_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("scan-single-table");
    auto engine =
        OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1, block_cache_bytes);
    if (!engine || !PutBatch(*engine, records, value_bytes)) {
      state.SkipWithError("prepare single-table Scan failed");
      return;
    }

    bool correct = true;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::size_t scan = 0; scan < scans && correct; ++scan)
      correct = engine->Scan(Key(records - returned), returned);
    const auto stopped = Clock::now();
    if (!correct) {
      state.SkipWithError("single-table Scan result mismatch");
      return;
    }
    const auto operations = returned * scans;
    SetReadMeasurements(state, operations, operations * (16 + value_bytes),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ScanMultiTable(benchmark::State& state, std::uint64_t records,
                    std::size_t returned, std::size_t scans, std::size_t value_bytes,
                    std::size_t block_cache_bytes) {
  constexpr std::uint64_t kTables = 4;
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("scan-multi-table");
    auto engine =
        OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1, block_cache_bytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }
    for (std::uint64_t table = 0; table < kTables; ++table) {
      if (!PutBatch(*engine, records / kTables, value_bytes, table * 2, kTables * 2)) {
        state.SkipWithError("prepare multi-table Scan failed");
        return;
      }
    }

    bool correct = true;
    const auto metrics_before = engine->ReadMetricsSnapshot();
    const auto started = Clock::now();
    for (std::size_t scan = 0; scan < scans && correct; ++scan)
      correct = engine->Scan(Key((records - returned) * 2), returned);
    const auto stopped = Clock::now();
    if (!correct) {
      state.SkipWithError("multi-table Scan result mismatch");
      return;
    }
    const auto operations = returned * scans;
    SetReadMeasurements(state, operations, operations * (16 + value_bytes),
                        std::chrono::duration<double>(stopped - started).count(),
                        metrics_before, engine->ReadMetricsSnapshot());
  }
}

void ConcurrentRead(benchmark::State& state, std::size_t thread_count,
                    std::uint64_t records, std::uint64_t total_reads,
                    std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("concurrent-read");
    auto engine = OpenEngine(EngineKind::kTinyLsm, directory.path(), false, 1);
    if (!engine || !PutBatch(*engine, records, value_bytes)) {
      state.SkipWithError("prepare concurrent-read SSTable failed");
      return;
    }

    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    std::vector<int> succeeded(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    const auto reads_per_thread = total_reads / thread_count;
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
      threads.emplace_back([&, thread] {
        std::uint64_t random = 0x5eedULL + thread;
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
          std::this_thread::yield();
        bool ok = true;
        for (std::uint64_t i = 0; i < reads_per_thread && ok; ++i)
          ok = engine->Get(Key(NextRandom(random) % records), true);
        succeeded[thread] = ok ? 1 : 0;
      });
    }
    while (ready.load(std::memory_order_acquire) != thread_count)
      std::this_thread::yield();
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
      thread.join();
    const auto stopped = Clock::now();
    if (std::find(succeeded.begin(), succeeded.end(), 0) != succeeded.end()) {
      state.SkipWithError("concurrent get failed");
      return;
    }

    const auto operations = reads_per_thread * thread_count;
    SetMeasurements(state, operations, operations * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void CompactManyTables(benchmark::State& state, std::uint64_t records,
                       std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("compact");
    auto engine = OpenEngine(EngineKind::kTinyLsm, directory.path(), false,
                             kCompactionMemtableBytes);
    if (!engine) {
      state.SkipWithError("open failed");
      return;
    }
    for (std::uint64_t i = 0; i < records; ++i) {
      if (!engine->Put(Key(i), Value(i, value_bytes))) {
        state.SkipWithError("prepare put failed");
        return;
      }
    }

    const auto started = Clock::now();
    const bool correct = engine->Compact();
    const auto stopped = Clock::now();
    if (!correct) {
      state.SkipWithError("compaction failed");
      return;
    }
    SetMeasurements(state, records, records * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
  }
}

void SnapshotScan(benchmark::State& state, bool pull_iterator, std::uint64_t records,
                  std::size_t scans, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(pull_iterator ? "snapshot-iterator" : "snapshot-scan");
    tinylsm::Options options;
    options.sync_on_write = false;
    options.memtable_bytes = 256U * 1024U * 1024U;
    options.max_pending_write_bytes = kSnapshotSetupQueueBytes;
    options.max_group_commit_bytes = kSnapshotSetupQueueBytes;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError("open Snapshot benchmark DB failed");
      return;
    }
    auto db = std::move(opened.value());
    for (std::uint64_t first = 0; first < records;
         first += kSnapshotSetupBatchOperations) {
      tinylsm::WriteBatch batch;
      const auto count = std::min(kSnapshotSetupBatchOperations, records - first);
      for (std::uint64_t offset = 0; offset < count; ++offset)
        batch.Put(Key(first + offset), Value(first + offset, value_bytes));
      if (!db->Write(batch).ok()) {
        state.SkipWithError("prepare Snapshot benchmark DB failed");
        return;
      }
    }
    auto snapshot = db->GetSnapshot();
    if (!snapshot.ok()) {
      state.SkipWithError("create Snapshot failed");
      return;
    }

    std::uint64_t first_result_nanoseconds = 0;
    std::uint64_t peak_rss_kb = 0;
    const auto started = Clock::now();
    for (std::size_t scan = 0; scan < scans; ++scan) {
      const auto first_started = Clock::now();
      if (pull_iterator) {
        auto iterator = db->NewIterator({}, {}, snapshot.value().get());
        if (!iterator.ok() || !iterator.value()->status().ok()) {
          state.SkipWithError("create Snapshot Iterator failed");
          return;
        }
        if (iterator.value()->Valid()) {
          first_result_nanoseconds += static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                   first_started)
                  .count());
        }
        std::uint64_t count = 0;
        while (iterator.value()->Valid()) {
          ++count;
          if (!iterator.value()->Next().ok()) {
            state.SkipWithError("advance Snapshot Iterator failed");
            return;
          }
        }
        if (!iterator.value()->status().ok() || count != records) {
          state.SkipWithError("Snapshot Iterator result mismatch");
          return;
        }
        peak_rss_kb = std::max(peak_rss_kb, ResidentMemoryKilobytes());
      } else {
        auto result = db->Scan({}, {});
        if (!result.ok() || result.value().size() != records) {
          state.SkipWithError("materialized Scan result mismatch");
          return;
        }
        peak_rss_kb = std::max(peak_rss_kb, ResidentMemoryKilobytes());
        first_result_nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                 first_started)
                .count());
      }
    }
    const auto stopped = Clock::now();
    const auto operations = records * scans;
    SetMeasurements(state, operations, operations * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["first_result_ns"] =
        static_cast<double>(first_result_nanoseconds) / static_cast<double>(scans);
    state.counters["snapshot_sequence"] =
        static_cast<double>(snapshot.value()->sequence());
    state.counters["peak_rss_kb"] = static_cast<double>(peak_rss_kb);
  }
}

void SnapshotRetention(benchmark::State& state, bool keep_snapshot,
                       std::size_t overwrite_generations, std::uint64_t records,
                       std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(keep_snapshot ? "snapshot-retained"
                                               : "snapshot-released");
    tinylsm::Options options;
    options.sync_on_write = false;
    options.memtable_bytes = 1;
    options.sstable_block_bytes = 4U * 1024U;
    options.compaction_table_trigger = 0;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError("open retention benchmark DB failed");
      return;
    }
    auto db = std::move(opened.value());
    tinylsm::WriteBatch base;
    for (std::uint64_t i = 0; i < records; ++i)
      base.Put(Key(i), Value(i, value_bytes));
    if (!db->Write(base).ok()) {
      state.SkipWithError("prepare retention base failed");
      return;
    }
    auto snapshot = db->GetSnapshot();
    if (!snapshot.ok()) {
      state.SkipWithError("create retention Snapshot failed");
      return;
    }
    for (std::size_t generation = 0; generation < overwrite_generations; ++generation) {
      tinylsm::WriteBatch overwrite;
      for (std::uint64_t i = 0; i < records; ++i) {
        overwrite.Put(Key(i), Value(i + (generation + 1) * records, value_bytes));
      }
      if (!db->Write(overwrite).ok() ||
          !db->Put("~rotate-" + std::to_string(generation), "x").ok()) {
        state.SkipWithError("prepare retention overwrite failed");
        return;
      }
    }
    if (!keep_snapshot)
      snapshot.value().reset();

    const auto started = Clock::now();
    if (!db->Compact().ok()) {
      state.SkipWithError("retention compaction failed");
      return;
    }
    const auto stopped = Clock::now();
    const auto metrics = db->GetSnapshotMetrics();
    const auto compaction_metrics = db->GetCompactionMetrics();
    if (keep_snapshot) {
      auto old = db->Get(Key(0), snapshot.value().get());
      if (!old.ok() || old.value() != Value(0, value_bytes)) {
        state.SkipWithError("retained Snapshot result mismatch");
        return;
      }
    }
    SetMeasurements(state, records * (overwrite_generations + 1),
                    records * (overwrite_generations + 1) * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["overwrite_generations"] =
        static_cast<double>(overwrite_generations);
    state.counters["active_snapshots"] = static_cast<double>(metrics.active_snapshots);
    state.counters["retained_versions"] =
        static_cast<double>(metrics.last_full_compaction_retained_versions);
    state.counters["retained_bytes"] =
        static_cast<double>(metrics.last_full_compaction_retained_bytes);
    state.counters["live_sstable_bytes"] =
        static_cast<double>(compaction_metrics.live_sstable_bytes);
  }
}

void SnapshotWriterOverlap(benchmark::State& state, bool pull_iterator,
                           std::uint64_t records, std::uint64_t writer_operations,
                           std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(pull_iterator ? "snapshot-overlap-iterator"
                                               : "snapshot-overlap-scan");
    tinylsm::Options options;
    options.sync_on_write = false;
    options.memtable_bytes = 256U * 1024U * 1024U;
    options.max_pending_write_bytes = kSnapshotSetupQueueBytes;
    options.max_group_commit_bytes = kSnapshotSetupQueueBytes;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError("open overlap benchmark DB failed");
      return;
    }
    auto db = std::move(opened.value());
    for (std::uint64_t first = 0; first < records;
         first += kSnapshotSetupBatchOperations) {
      tinylsm::WriteBatch batch;
      const auto count = std::min(kSnapshotSetupBatchOperations, records - first);
      for (std::uint64_t offset = 0; offset < count; ++offset)
        batch.Put(Key(first + offset), Value(first + offset, value_bytes));
      if (!db->Write(batch).ok()) {
        state.SkipWithError("prepare overlap benchmark DB failed");
        return;
      }
    }
    auto snapshot = db->GetSnapshot();
    if (!snapshot.ok()) {
      state.SkipWithError("create overlap Snapshot failed");
      return;
    }

    const auto before = db->GetReadMetrics();
    std::atomic<bool> start = false;
    std::atomic<bool> reader_ok = true;
    std::atomic<bool> writer_ok = true;
    std::vector<double> writer_latencies;
    writer_latencies.reserve(static_cast<std::size_t>(writer_operations));
    std::thread reader([&] {
      start.wait(false);
      if (pull_iterator) {
        auto iterator = db->NewIterator({}, {}, snapshot.value().get());
        if (!iterator.ok()) {
          reader_ok.store(false, std::memory_order_relaxed);
          return;
        }
        std::uint64_t count = 0;
        while (iterator.value()->Valid()) {
          ++count;
          if (!iterator.value()->Next().ok()) {
            reader_ok.store(false, std::memory_order_relaxed);
            return;
          }
        }
        if (!iterator.value()->status().ok() || count != records)
          reader_ok.store(false, std::memory_order_relaxed);
        return;
      }

      auto scan = db->Scan({}, {});
      if (!scan.ok() || scan.value().size() != records)
        reader_ok.store(false, std::memory_order_relaxed);
    });
    std::thread writer([&] {
      start.wait(false);
      for (std::uint64_t i = 0; i < writer_operations; ++i) {
        const auto write_started = Clock::now();
        if (!db->Put(Key(records + i), Value(records + i, value_bytes)).ok()) {
          writer_ok.store(false, std::memory_order_relaxed);
          return;
        }
        writer_latencies.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - write_started)
                .count());
      }
    });

    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    start.notify_all();
    reader.join();
    writer.join();
    const auto stopped = Clock::now();
    if (!reader_ok.load(std::memory_order_relaxed) ||
        !writer_ok.load(std::memory_order_relaxed) ||
        writer_latencies.size() != writer_operations) {
      state.SkipWithError("overlap benchmark result mismatch");
      return;
    }

    const auto after = db->GetReadMetrics();
    SetMeasurements(state, records + writer_operations,
                    (records + writer_operations) * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["writer_p50_us"] = Percentile(writer_latencies, 0.50);
    state.counters["writer_p95_us"] = Percentile(writer_latencies, 0.95);
    state.counters["writer_p99_us"] = Percentile(writer_latencies, 0.99);
    state.counters["write_lock_wait_ns"] = static_cast<double>(
        after.write_lock_wait_nanoseconds - before.write_lock_wait_nanoseconds);
    state.counters["write_lock_acquisitions"] = static_cast<double>(
        after.write_lock_acquisitions - before.write_lock_acquisitions);
  }
}

enum class SstableKeyShape { kSharedPrefix, kRandom };

std::vector<tinylsm::internal::InternalEntry>
MakeSstableEntries(SstableKeyShape shape, std::size_t entries,
                   std::size_t value_bytes) {
  std::vector<tinylsm::internal::InternalEntry> out;
  out.reserve(entries);
  std::uint64_t random = 0x51a7e5eedULL;
  for (std::size_t index = 0; index < entries; ++index) {
    std::string key;
    if (shape == SstableKeyShape::kSharedPrefix) {
      key = "tenant/000042/collection/records/" + Key(index);
    } else {
      std::ostringstream encoded;
      encoded << 'r' << std::hex << std::setw(16) << std::setfill('0')
              << NextRandom(random);
      key = encoded.str();
    }
    out.push_back({std::move(key), static_cast<std::uint64_t>(index + 1),
                   tinylsm::internal::ValueType::kValue, Value(index, value_bytes)});
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return left.user_key < right.user_key;
  });
  return out;
}

void SstableDataBlockEncode(benchmark::State& state, bool v2, SstableKeyShape shape,
                            std::uint32_t restart_interval, std::size_t entries,
                            std::size_t value_bytes) {
  const auto input = MakeSstableEntries(shape, entries, value_bytes);
  for (auto _ : state) {
    (void)_;
    const auto started = Clock::now();
    auto encoded = v2 ? tinylsm::internal::EncodeDataBlock(input, restart_interval)
                      : tinylsm::internal::EncodeDataBlockV1(input);
    const auto stopped = Clock::now();
    if (!encoded.ok()) {
      state.SkipWithError(encoded.status().ToString().c_str());
      return;
    }
    SetMeasurements(state, entries, entries * (32 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["encoded_bytes"] = static_cast<double>(encoded.value().size());
    state.counters["bytes_per_entry"] =
        static_cast<double>(encoded.value().size()) / static_cast<double>(entries);
    state.counters["restart_interval"] =
        v2 ? static_cast<double>(restart_interval) : 0.0;
  }
}

void SstableDataBlockLookup(benchmark::State& state, bool v2, SstableKeyShape shape,
                            std::uint32_t restart_interval, std::size_t entries,
                            std::uint64_t lookups, std::size_t value_bytes) {
  const auto input = MakeSstableEntries(shape, entries, value_bytes);
  auto encoded = v2 ? tinylsm::internal::EncodeDataBlock(input, restart_interval)
                    : tinylsm::internal::EncodeDataBlockV1(input);
  if (!encoded.ok()) {
    state.SkipWithError(encoded.status().ToString().c_str());
    return;
  }
  const auto version =
      v2 ? tinylsm::internal::kSstableVersion : tinylsm::internal::kSstableVersionV1;

  for (auto _ : state) {
    (void)_;
    std::uint64_t random = 0xdecafbadULL;
    const auto started = Clock::now();
    for (std::uint64_t lookup = 0; lookup < lookups; ++lookup) {
      const auto& expected = input[NextRandom(random) % input.size()];
      auto found = tinylsm::internal::FindDataBlockEntry(
          tinylsm::internal::AsBytes(encoded.value()), version, expected.user_key,
          std::numeric_limits<std::uint64_t>::max());
      if (!found.ok() || found.value().value != expected.value) {
        state.SkipWithError("SSTable data-block lookup result mismatch");
        return;
      }
    }
    const auto stopped = Clock::now();
    SetMeasurements(state, lookups, lookups * (32 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["block_bytes"] = static_cast<double>(encoded.value().size());
    state.counters["bytes_per_entry"] =
        static_cast<double>(encoded.value().size()) / static_cast<double>(entries);
    state.counters["restart_interval"] =
        v2 ? static_cast<double>(restart_interval) : 0.0;
  }
}

void GroupCommitMultiWriter(benchmark::State& state, std::size_t writer_count,
                            std::size_t max_group_requests,
                            std::uint64_t total_operations, std::size_t value_bytes) {
  if (total_operations % writer_count != 0) {
    state.SkipWithError("total group-commit operations must divide across writers");
    return;
  }
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory("group-commit-" + std::to_string(writer_count) + "-" +
                                 std::to_string(max_group_requests));
    tinylsm::Options options;
    options.sync_on_write = true;
    options.memtable_bytes = kGroupCommitMemtableBytes;
    options.max_pending_write_requests = total_operations;
    options.max_pending_write_bytes = 64U * 1024U * 1024U;
    options.max_group_commit_requests = max_group_requests;
    options.max_group_commit_bytes = 1U * 1024U * 1024U;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError(opened.status().ToString().c_str());
      return;
    }
    auto db = std::move(opened.value());
    const auto operations_per_writer = total_operations / writer_count;
    std::vector<std::vector<double>> latencies(writer_count);
    std::atomic<bool> writers_ok = true;
    std::barrier start(static_cast<std::ptrdiff_t>(writer_count + 1));
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
      writers.emplace_back([&, writer] {
        auto& samples = latencies[writer];
        samples.reserve(static_cast<std::size_t>(operations_per_writer));
        start.arrive_and_wait();
        for (std::uint64_t index = 0; index < operations_per_writer; ++index) {
          const auto operation_started = Clock::now();
          const auto key =
              "writer-" + std::to_string(writer) + "-" + std::to_string(index);
          if (!db->Put(key, Value(index, value_bytes)).ok()) {
            writers_ok.store(false, std::memory_order_relaxed);
            return;
          }
          samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() -
                                                                      operation_started)
                                .count());
        }
      });
    }

    const auto started = Clock::now();
    start.arrive_and_wait();
    for (auto& writer : writers)
      writer.join();
    const auto stopped = Clock::now();
    if (!writers_ok.load(std::memory_order_relaxed)) {
      state.SkipWithError("group-commit writer failed");
      return;
    }
    std::vector<double> combined_latencies;
    combined_latencies.reserve(static_cast<std::size_t>(total_operations));
    for (const auto& samples : latencies)
      combined_latencies.insert(combined_latencies.end(), samples.begin(),
                                samples.end());
    if (combined_latencies.size() != total_operations) {
      state.SkipWithError("group-commit sample count mismatch");
      return;
    }
    const auto metrics = db->GetWriteMetrics();
    if (!db->Close().ok()) {
      state.SkipWithError("close group-commit benchmark DB failed");
      return;
    }

    SetMeasurements(state, total_operations, total_operations * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["writer_count"] = static_cast<double>(writer_count);
    state.counters["max_group_requests"] = static_cast<double>(max_group_requests);
    state.counters["physical_groups"] = static_cast<double>(metrics.group_commits);
    state.counters["grouped_requests"] =
        static_cast<double>(metrics.grouped_write_requests);
    state.counters["wal_syncs"] = static_cast<double>(metrics.wal_syncs);
    state.counters["wal_syncs_per_write"] =
        static_cast<double>(metrics.wal_syncs) / static_cast<double>(total_operations);
    state.counters["writer_queue_wait_us"] =
        static_cast<double>(metrics.writer_queue_wait_nanoseconds) / 1'000.0;
    state.counters["max_writer_queue_depth"] =
        static_cast<double>(metrics.max_writer_queue_depth);
    state.counters["put_p50_us"] = Percentile(combined_latencies, 0.50);
    state.counters["put_p95_us"] = Percentile(combined_latencies, 0.95);
    state.counters["put_p99_us"] = Percentile(combined_latencies, 0.99);
  }
}

void CompactionMixed(benchmark::State& state, bool background, std::uint64_t operations,
                     std::size_t key_space, std::size_t value_bytes) {
  for (auto _ : state) {
    (void)_;
    TemporaryDirectory directory(background ? "compact-mixed-background"
                                            : "compact-mixed-manual");
    tinylsm::Options options;
    options.sync_on_write = false;
    options.memtable_bytes = 64U * 1024U;
    options.block_cache_bytes = 0;
    options.compaction_table_trigger = background ? 4 : 0;
    auto opened = tinylsm::DB::Open(directory.path(), options);
    if (!opened.ok()) {
      state.SkipWithError("open failed");
      return;
    }

    auto db = std::move(opened.value());
    std::vector<bool> live(key_space);
    std::vector<double> write_latencies;
    std::vector<double> read_latencies;
    std::vector<double> scan_latencies;
    write_latencies.reserve(static_cast<std::size_t>(operations));
    read_latencies.reserve(static_cast<std::size_t>(operations / 5));
    scan_latencies.reserve(static_cast<std::size_t>(operations / 100));
    std::uint64_t writes = 0;
    std::uint64_t reads = 0;
    std::uint64_t scans = 0;
    std::uint64_t random = 0x5eedULL;

    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < operations; ++i) {
      const auto random_key = static_cast<std::size_t>(NextRandom(random) % key_space);
      const auto key_index = i % 3 == 0   ? static_cast<std::size_t>(i % key_space)
                             : i % 3 == 1 ? random_key % 64
                                          : random_key;
      const auto operation_started = Clock::now();
      tinylsm::Status status;
      switch (i % 100) {
      case 0:
        status = db->Delete(Key(key_index));
        live[key_index] = false;
        break;
      case 98: {
        auto result = db->Get(Key(key_index));
        if (!result.ok() && result.status().code() != tinylsm::StatusCode::kNotFound) {
          state.SkipWithError(result.status().ToString().c_str());
          return;
        }
        read_latencies.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - operation_started)
                .count());
        ++reads;
        continue;
      }
      case 99: {
        const auto begin = Key(key_index / 2);
        const auto end = Key(std::min(key_space, key_index / 2 + 128));
        auto result = db->Scan(begin, end);
        if (!result.ok()) {
          state.SkipWithError(result.status().ToString().c_str());
          return;
        }
        scan_latencies.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - operation_started)
                .count());
        ++scans;
        continue;
      }
      default:
        status = db->Put(Key(key_index), Value(i, value_bytes));
        live[key_index] = true;
        break;
      }
      if (!status.ok()) {
        state.SkipWithError(status.ToString().c_str());
        return;
      }
      write_latencies.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - operation_started)
              .count());
      ++writes;
    }
    const auto stopped = Clock::now();

    if (!background && !db->Compact().ok()) {
      state.SkipWithError("manual compaction failed");
      return;
    }
    if (!db->Close().ok()) {
      state.SkipWithError("close failed");
      return;
    }

    const auto read_metrics = db->GetReadMetrics();
    const auto write_metrics = db->GetWriteMetrics();
    const auto compaction_metrics = db->GetCompactionMetrics();
    std::uint64_t logical_live_bytes = 0;
    for (std::size_t i = 0; i < key_space; ++i) {
      if (live[i])
        logical_live_bytes += Key(i).size() + value_bytes;
    }

    SetMeasurements(state, operations, writes * (16 + value_bytes),
                    std::chrono::duration<double>(stopped - started).count());
    state.counters["foreground_writes"] = static_cast<double>(writes);
    state.counters["foreground_reads"] = static_cast<double>(reads);
    state.counters["foreground_scans"] = static_cast<double>(scans);
    state.counters["put_p50_us"] = Percentile(write_latencies, 0.50);
    state.counters["put_p95_us"] = Percentile(write_latencies, 0.95);
    state.counters["put_p99_us"] = Percentile(write_latencies, 0.99);
    state.counters["get_p50_us"] = Percentile(read_latencies, 0.50);
    state.counters["get_p95_us"] = Percentile(read_latencies, 0.95);
    state.counters["get_p99_us"] = Percentile(read_latencies, 0.99);
    state.counters["scan_p50_us"] = Percentile(scan_latencies, 0.50);
    state.counters["scan_p95_us"] = Percentile(scan_latencies, 0.95);
    state.counters["scan_p99_us"] = Percentile(scan_latencies, 0.99);
    state.counters["point_lookups"] = static_cast<double>(read_metrics.point_lookups);
    state.counters["table_probes"] = static_cast<double>(read_metrics.table_probes);
    state.counters["scan_table_inputs"] =
        static_cast<double>(read_metrics.scan_table_inputs);
    state.counters["logical_write_bytes"] =
        static_cast<double>(write_metrics.logical_write_bytes);
    state.counters["logical_live_bytes"] = static_cast<double>(logical_live_bytes);
    state.counters["flush_output_bytes"] =
        static_cast<double>(compaction_metrics.flush_output_bytes);
    state.counters["compaction_input_bytes"] =
        static_cast<double>(compaction_metrics.compaction_input_bytes);
    state.counters["compaction_output_bytes"] =
        static_cast<double>(compaction_metrics.compaction_output_bytes);
    state.counters["compactions"] = static_cast<double>(compaction_metrics.compactions);
    state.counters["background_compactions"] =
        static_cast<double>(compaction_metrics.background_compactions);
    state.counters["table_count"] = static_cast<double>(compaction_metrics.table_count);
    state.counters["live_sstable_bytes"] =
        static_cast<double>(compaction_metrics.live_sstable_bytes);
    state.counters["compaction_debt_tables"] =
        static_cast<double>(compaction_metrics.compaction_debt_tables);
    state.counters["compaction_debt_bytes"] =
        static_cast<double>(compaction_metrics.compaction_debt_bytes);
    state.counters["read_amplification"] =
        read_metrics.point_lookups == 0
            ? 0.0
            : static_cast<double>(read_metrics.table_probes) /
                  static_cast<double>(read_metrics.point_lookups);
    state.counters["write_amplification"] =
        write_metrics.logical_write_bytes == 0
            ? 0.0
            : static_cast<double>(compaction_metrics.flush_output_bytes +
                                  compaction_metrics.compaction_output_bytes) /
                  static_cast<double>(write_metrics.logical_write_bytes);
    state.counters["space_amplification"] =
        logical_live_bytes == 0
            ? 0.0
            : static_cast<double>(compaction_metrics.live_sstable_bytes) /
                  static_cast<double>(logical_live_bytes);
  }
}

void RegisterCommon(std::string_view prefix, EngineKind kind) {
  benchmark::RegisterBenchmark((std::string(prefix) + "/FillSequentialAsync").c_str(),
                               FillSequential, kind, false, 100'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark((std::string(prefix) + "/FillSequentialSync").c_str(),
                               FillSequential, kind, true, 1'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark((std::string(prefix) + "/ReadRandomHit").c_str(),
                               ReadRandom, kind, true, 100'000, 100'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark((std::string(prefix) + "/ReadRandomMiss").c_str(),
                               ReadRandom, kind, false, 100'000, 100'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark((std::string(prefix) + "/ScanTail10000Repeated").c_str(),
                               ScanTail, kind, 100'000, 10'000, 20, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  for (const std::size_t batch_size : {1U, 10U, 100U}) {
    benchmark::RegisterBenchmark(
        (std::string(prefix) + "/FillBatch" + std::to_string(batch_size)).c_str(),
        FillBatch, kind, 1'000, batch_size, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
  for (const std::size_t threads : {1U, 2U, 4U, 8U}) {
    benchmark::RegisterBenchmark(
        (std::string(prefix) + "/ConcurrentFill" + std::to_string(threads)).c_str(),
        ConcurrentFill, kind, threads, 10'000, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
}

const bool registered = [] {
  RegisterCommon("TinyLSM", EngineKind::kTinyLsm);
#ifdef TINYLSM_HAVE_LEVELDB
  RegisterCommon("LevelDB", EngineKind::kLevelDb);
#endif
  benchmark::RegisterBenchmark("TinyLSM/WriteFlushLatencyAsync", WriteFlushLatency,
                               false, 100'000, 256, 256U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/WriteFlushLatencySync", WriteFlushLatency, true,
                               20'000, 256, 256U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadMemTableHit", ReadMemTableHit, 25'000,
                               200'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadSingleTableHit", ReadSingleTableHit, 25'000,
                               50'000, 100, 8U * 1024U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadMultiTableHit", ReadMultiTable, true,
                               25'000, 25'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadMultiTableMiss", ReadMultiTable, false,
                               25'000, 25'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadRepeatedHit", ReadRepeatedHit, 25'000, 256,
                               50'000, 100, 8U * 1024U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ScanSingleTableRepeated", ScanSingleTable,
                               25'000, 10'000, 20, 100, 8U * 1024U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ScanMultiTableRepeated", ScanMultiTable, 25'000,
                               10'000, 20, 100, 8U * 1024U * 1024U)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  // This long, single-repetition case exists for process-wide profilers. It is
  // excluded from the fixed before/after result filter.
  benchmark::RegisterBenchmark("TinyLSM/ScanProfile", ScanMultiTable, 25'000, 10'000,
                               500, 100, 0)
      ->Iterations(1)
      ->Repetitions(1)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ReadRepeatedHitCacheDisabled", ReadRepeatedHit,
                               25'000, 256, 50'000, 100, 0)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/ScanMultiTableCacheDisabled", ScanMultiTable,
                               25'000, 10'000, 20, 100, 0)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  for (const std::size_t threads : {1U, 2U, 4U, 8U}) {
    benchmark::RegisterBenchmark(
        ("TinyLSM/ConcurrentRead" + std::to_string(threads)).c_str(), ConcurrentRead,
        threads, 25'000, 40'000, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
  benchmark::RegisterBenchmark("TinyLSM/CompactManyTables", CompactManyTables, 100'000,
                               100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  for (const std::uint64_t records : {10'000ULL, 100'000ULL, 1'000'000ULL}) {
    const auto label = std::to_string(records / 1'000) + "K";
    benchmark::RegisterBenchmark(("TinyLSM/SnapshotMaterializedScan" + label).c_str(),
                                 SnapshotScan, false, records, 10, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
    benchmark::RegisterBenchmark(("TinyLSM/SnapshotIterator" + label).c_str(),
                                 SnapshotScan, true, records, 10, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
  for (const std::size_t generations : {0U, 1U, 6U}) {
    const auto label = std::to_string(generations == 0 ? 0 : generations * 10) + "s";
    benchmark::RegisterBenchmark(("TinyLSM/SnapshotRetentionHeld" + label).c_str(),
                                 SnapshotRetention, true, generations, 2'000, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
  benchmark::RegisterBenchmark("TinyLSM/SnapshotRetentionReleased60s",
                               SnapshotRetention, false, 6, 2'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/SnapshotWriterOverlapMaterializedScan",
                               SnapshotWriterOverlap, false, 100'000, 5'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/SnapshotWriterOverlapIterator",
                               SnapshotWriterOverlap, true, 100'000, 5'000, 100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  for (const auto [shape, raw_label] :
       {std::pair{SstableKeyShape::kSharedPrefix, "Prefix"},
        std::pair{SstableKeyShape::kRandom, "Random"}}) {
    const std::string label(raw_label);
    benchmark::RegisterBenchmark(("TinyLSM/SstableDataBlockEncodeV1" + label).c_str(),
                                 SstableDataBlockEncode, false, shape, 0, 16'384, 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
    benchmark::RegisterBenchmark(("TinyLSM/SstableDataBlockLookupV1" + label).c_str(),
                                 SstableDataBlockLookup, false, shape, 0, 128, 2'000,
                                 100)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
    for (const std::uint32_t restart_interval : {4U, 16U, 64U}) {
      const auto suffix = "V2" + label + "Restart" + std::to_string(restart_interval);
      benchmark::RegisterBenchmark(("TinyLSM/SstableDataBlockEncode" + suffix).c_str(),
                                   SstableDataBlockEncode, true, shape,
                                   restart_interval, 16'384, 100)
          ->Iterations(1)
          ->Repetitions(5)
          ->UseManualTime();
      benchmark::RegisterBenchmark(("TinyLSM/SstableDataBlockLookup" + suffix).c_str(),
                                   SstableDataBlockLookup, true, shape,
                                   restart_interval, 128, 2'000, 100)
          ->Iterations(1)
          ->Repetitions(5)
          ->UseManualTime();
    }
  }
  for (const std::size_t writers : {1U, 2U, 4U, 8U, 16U}) {
    const auto writer_label = std::to_string(writers) + "Writers";
    benchmark::RegisterBenchmark(("TinyLSM/GroupCommitOff" + writer_label).c_str(),
                                 GroupCommitMultiWriter, writers, 1, 8'192, 256)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
    benchmark::RegisterBenchmark(("TinyLSM/GroupCommitOn" + writer_label).c_str(),
                                 GroupCommitMultiWriter, writers, 8, 8'192, 256)
        ->Iterations(1)
        ->Repetitions(5)
        ->UseManualTime();
  }
  benchmark::RegisterBenchmark("TinyLSM/CompactionMixedManual", CompactionMixed, false,
                               20'000, 4'096, 256)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  benchmark::RegisterBenchmark("TinyLSM/CompactionMixedSizeTiered", CompactionMixed,
                               true, 20'000, 4'096, 256)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  return true;
}();

} // namespace

BENCHMARK_MAIN();
