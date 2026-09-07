#include <benchmark/benchmark.h>

#include <chrono>
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
};

class TinyLsmEngine final : public Engine {
public:
  static std::unique_ptr<TinyLsmEngine> Open(const std::filesystem::path& path,
                                             bool sync, std::size_t memtable_bytes) {
    tinylsm::Options options;
    options.sync_on_write = sync;
    options.memtable_bytes = memtable_bytes;
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

private:
  explicit TinyLsmEngine(std::unique_ptr<tinylsm::DB> db) : db_(std::move(db)) {}
  std::unique_ptr<tinylsm::DB> db_;
};

#ifdef TINYLSM_HAVE_LEVELDB
class LevelDbEngine final : public Engine {
public:
  static std::unique_ptr<LevelDbEngine> Open(const std::filesystem::path& path,
                                             bool sync, std::size_t) {
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

private:
  LevelDbEngine(leveldb::DB* db, bool sync) : db_(db) { write_options_.sync = sync; }

  std::unique_ptr<leveldb::DB> db_;
  leveldb::WriteOptions write_options_;
};
#endif

enum class EngineKind { kTinyLsm, kLevelDb };

std::unique_ptr<Engine> OpenEngine(EngineKind kind, const std::filesystem::path& path,
                                   bool sync, std::size_t memtable_bytes) {
  if (kind == EngineKind::kTinyLsm)
    return TinyLsmEngine::Open(path, sync, memtable_bytes);
#ifdef TINYLSM_HAVE_LEVELDB
  return LevelDbEngine::Open(path, sync, memtable_bytes);
#else
  (void)path;
  (void)sync;
  (void)memtable_bytes;
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

void ScanTail(benchmark::State& state, EngineKind kind, std::uint64_t records,
              std::size_t returned, std::size_t value_bytes) {
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
    const bool correct = engine->Scan(Key(records - returned), returned);
    const auto stopped = Clock::now();
    if (!correct) {
      state.SkipWithError("scan result mismatch");
      return;
    }
    SetMeasurements(state, returned, returned * (16 + value_bytes),
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
  benchmark::RegisterBenchmark((std::string(prefix) + "/ScanTail1000").c_str(),
                               ScanTail, kind, 100'000, 1'000, 100)
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
  benchmark::RegisterBenchmark("TinyLSM/CompactManyTables", CompactManyTables, 100'000,
                               100)
      ->Iterations(1)
      ->Repetitions(5)
      ->UseManualTime();
  return true;
}();

} // namespace

BENCHMARK_MAIN();
