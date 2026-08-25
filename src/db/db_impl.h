#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "io/file.h"
#include "manifest/manifest_state.h"
#include "memtable/memtable.h"
#include "sstable/sstable_reader.h"
#include "tinylsm/db.h"
#include "wal/wal_writer.h"

namespace tinylsm {

class DB::Impl {
public:
  static Result<std::unique_ptr<Impl>> OpenInMemory();
  static Result<std::unique_ptr<Impl>> Open(const std::filesystem::path& path, Options options);
  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  Result<std::string> Get(std::string_view key) const;
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;
  Status Close();
  ~Impl();

private:
  Impl() = default;
  Status Write(std::string_view key, std::string_view value, internal::ValueType type);
  Status FlushMemTable();
  Status CheckOpen() const;

  Options options_;
  std::optional<std::filesystem::path> path_;
  std::unique_ptr<internal::FileSystem> fs_;
  internal::MemTable memtable_;
  std::unique_ptr<internal::WalWriter> wal_;
  std::unique_ptr<internal::SSTableReader> table_;
  std::unique_ptr<internal::ManifestState> manifest_;
  std::uint64_t next_sequence_ = 1;
  bool closed_ = false;
};

} // namespace tinylsm
