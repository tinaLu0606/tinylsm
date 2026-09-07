#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "tinylsm/db.h"
#include "tinylsm/read_metrics.h"
#include "tinylsm/write_batch.h"

#ifdef TINYLSM_HAVE_LEVELDB
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kDefaultMemtableBytes = 4U * 1024U * 1024U;
constexpr std::size_t kCompactionMemtableBytes = 512U * 1024U;

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
  benchmark::RegisterBenchmark("TinyLSM/WriteFlushLatencySync", WriteFlushLatency,
                               true, 20'000, 256, 256U * 1024U)
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
  return true;
}();

} // namespace

BENCHMARK_MAIN();
