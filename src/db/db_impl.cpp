#include "db/db_impl.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

#include "sstable/sstable_builder.h"
#include "util/bytewise_less.h"
#include "wal/wal_reader.h"

namespace tinylsm {
namespace {
std::string Numbered(std::uint64_t n, std::string_view suffix) {
  std::ostringstream out;
  out << std::setw(6) << std::setfill('0') << n << suffix;
  return out.str();
}
std::string WalName(std::uint64_t n) { return Numbered(n, ".wal"); }
std::string SstName(std::uint64_t n) { return Numbered(n, ".sst"); }
internal::DecodeLimits Limits(const Options& o) {
  return {o.max_key_bytes, o.max_value_bytes};
}
} // namespace

Result<std::unique_ptr<DB::Impl>> DB::Impl::OpenInMemory() {
  return std::unique_ptr<Impl>(new Impl());
}

Result<std::unique_ptr<DB::Impl>> DB::Impl::Open(const std::filesystem::path& path,
                                                 Options options) {
  return Open(path, std::move(options), internal::NewPosixFileSystem());
}

Result<std::unique_ptr<DB::Impl>>
DB::Impl::Open(const std::filesystem::path& path, Options options,
               std::unique_ptr<internal::FileSystem> fs) {
  if (path.empty())
    return Status::InvalidArgument("database path is empty");
  if (options.memtable_bytes == 0 || options.sstable_block_bytes == 0)
    return Status::InvalidArgument("size options must be non-zero");
  if (!fs)
    return Status::InvalidArgument("filesystem is null");

  auto impl = std::unique_ptr<Impl>(new Impl());
  impl->options_ = options;
  impl->path_ = path;
  impl->fs_ = std::move(fs);

  auto s = impl->EnsureDatabaseDirectory();
  if (!s.ok())
    return s;

  const auto manifest_path = path / "MANIFEST";
  auto manifest_exists = impl->fs_->FileExists(manifest_path);
  if (!manifest_exists.ok())
    return manifest_exists.status().WithContext("inspect MANIFEST");

  auto loaded =
      manifest_exists.value() ? impl->LoadManifest() : impl->CreateInitialManifest();
  if (!loaded.ok())
    return loaded.status();
  auto snapshot = std::move(loaded.value());

  impl->manifest_ =
      std::make_unique<internal::ManifestState>(*impl->fs_, path, snapshot);

  s = impl->OpenManifestSSTable(snapshot);
  if (!s.ok())
    return s;

  s = impl->RecoverActiveWal(snapshot);
  if (!s.ok())
    return s;

  return impl;
}

Status DB::Impl::EnsureDatabaseDirectory() {
  auto path_exists = fs_->FileExists(*path_);
  if (!path_exists.ok())
    return path_exists.status().WithContext("open database");
  if (path_exists.value())
    return Status::Ok();

  if (!options_.create_if_missing)
    return Status::NotFound("database directory does not exist");

  auto s = fs_->CreateDir(*path_);
  return s.ok() ? s : s.WithContext("create database directory");
}

Result<internal::ManifestSnapshot> DB::Impl::LoadManifest() {
  auto loaded = internal::ManifestState::Load(*fs_, *path_);
  if (!loaded.ok())
    return loaded.status().WithContext("load MANIFEST");
  return std::move(loaded.value());
}

Result<internal::ManifestSnapshot> DB::Impl::CreateInitialManifest() {
  if (!options_.create_if_missing)
    return Status::NotFound("database manifest does not exist");

  internal::ManifestSnapshot snapshot;
  snapshot.active_wal_number = 1;
  snapshot.next_file_number = 2;

  auto initial = fs_->OpenWritable(*path_ / WalName(1), false);
  if (!initial.ok())
    return initial.status().WithContext("create initial WAL");

  auto s = initial.value()->Sync();
  if (!s.ok())
    return s.WithContext("sync initial WAL");

  s = initial.value()->Close();
  if (!s.ok())
    return s.WithContext("close initial WAL");

  internal::ManifestState state(*fs_, *path_, snapshot);
  auto published = state.Publish(snapshot);
  if (!published.durable()) {
    const auto context =
        published.state() == internal::ManifestPublishState::kVisibleNotDurable
            ? "publish initial MANIFEST: replacement may be visible; retry DB::Open"
            : "publish initial MANIFEST";
    return published.status().WithContext(context);
  }

  return snapshot;
}

Status DB::Impl::OpenManifestSSTable(const internal::ManifestSnapshot& snapshot) {
  if (!snapshot.live_table)
    return Status::Ok();

  const auto sst_path = *path_ / SstName(snapshot.live_table->file_number);
  auto table_exists = fs_->FileExists(sst_path);
  if (!table_exists.ok())
    return table_exists.status().WithContext("inspect Manifest SSTable");
  if (!table_exists.value())
    return Status::Corruption("manifest references a missing SSTable");

  auto file = fs_->OpenRandomAccess(sst_path);
  if (!file.ok())
    return file.status().WithContext("open Manifest SSTable");

  auto actual_size = file.value()->Size();
  if (!actual_size.ok())
    return actual_size.status().WithContext("read Manifest SSTable size");
  if (actual_size.value() != snapshot.live_table->file_size)
    return Status::Corruption("manifest SSTable size does not match file");

  auto reader = internal::SSTableReader::Open(std::move(file.value()));
  if (!reader.ok())
    return reader.status().WithContext("validate Manifest SSTable");

  table_ = std::move(reader.value());
  return Status::Ok();
}

Status DB::Impl::RecoverActiveWal(const internal::ManifestSnapshot& snapshot) {
  const auto wal_path = *path_ / WalName(snapshot.active_wal_number);
  auto wal_exists = fs_->FileExists(wal_path);
  if (!wal_exists.ok())
    return wal_exists.status().WithContext("inspect active WAL");
  if (!wal_exists.value())
    return Status::Corruption("manifest references a missing WAL");

  auto seq = fs_->OpenSequential(wal_path);
  if (!seq.ok())
    return seq.status().WithContext("open active WAL for replay");

  internal::WalReader reader(std::move(seq.value()), Limits(options_));
  auto replay = reader.Replay(
      [&](const internal::InternalEntry& e) { return memtable_.Apply(e); });
  if (!replay.ok())
    return replay.status().WithContext("replay active WAL");

  if (replay.value().truncated_tail) {
    auto s = fs_->Truncate(wal_path, replay.value().valid_bytes);
    if (!s.ok())
      return s.WithContext("truncate incomplete WAL tail");
  }

  const auto max_seq = std::max(snapshot.last_sequence, replay.value().max_sequence);
  if (max_seq == std::numeric_limits<std::uint64_t>::max())
    return Status::ResourceExhausted("sequence space is exhausted");
  next_sequence_ = max_seq + 1;

  auto writable = fs_->OpenWritable(wal_path, true);
  if (!writable.ok())
    return writable.status().WithContext("open active WAL for append");
  wal_ = std::make_unique<internal::WalWriter>(std::move(writable.value()),
                                               Limits(options_));
  return Status::Ok();
}

Status DB::Impl::CheckOpen() const {
  if (closed_)
    return Status::AlreadyClosed("database is closed");
  return terminal_error_.value_or(Status::Ok());
}
Status DB::Impl::Put(std::string_view k, std::string_view v) {
  return Write(k, v, internal::ValueType::kValue);
}
Status DB::Impl::Delete(std::string_view k) {
  return Write(k, {}, internal::ValueType::kTombstone);
}
Status DB::Impl::Write(std::string_view key, std::string_view value,
                       internal::ValueType type) {
  auto open = CheckOpen();
  if (!open.ok())
    return open;
  if (key.size() > options_.max_key_bytes || value.size() > options_.max_value_bytes)
    return Status::InvalidArgument("key or value exceeds configured limit");

  std::size_t projected = memtable_.ApproximateMemoryUsage();
  auto existing = memtable_.Get(key);
  if (existing.ok())
    projected -= sizeof(internal::InternalEntry) + existing.value().user_key.size() +
                 existing.value().value.size();
  const std::size_t added = sizeof(internal::InternalEntry) + key.size() + value.size();
  if (added > std::numeric_limits<std::size_t>::max() - projected)
    return Status::ResourceExhausted("memtable size accounting overflow");
  projected += added;

  if (manifest_ && manifest_->current().live_table &&
      projected >= options_.memtable_bytes)
    return Status::NotSupported("V2 supports only one flushed SSTable");
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
    return Status::ResourceExhausted("sequence space is exhausted");

  internal::InternalEntry entry{std::string(key), next_sequence_++, type,
                                std::string(value)};

  if (wal_) {
    auto s = wal_->Append(entry);
    if (!s.ok())
      return s;
    if (options_.sync_on_write) {
      s = wal_->Sync();
      if (!s.ok())
        return s;
    }
  }

  auto s = memtable_.Apply(std::move(entry));
  if (!s.ok())
    return s;

  if (manifest_ && memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes) {
    s = FlushMemTable();
    if (!s.ok())
      return s;
  }
  return Status::Ok();
}

Status DB::Impl::FlushMemTable() {
  const auto current = manifest_->current();
  if (current.live_table)
    return Status::NotSupported("V2 supports only one flushed SSTable");

  const std::uint64_t table_number = current.next_file_number;
  if (table_number >= std::numeric_limits<std::uint64_t>::max() - 1)
    return Status::ResourceExhausted("file number space is exhausted");
  const std::uint64_t wal_number = table_number + 1;

  const auto temp = *path_ / (SstName(table_number) + ".tmp"),
             final = *path_ / SstName(table_number);
  auto file = fs_->OpenWritable(temp, false);
  if (!file.ok())
    return file.status().WithContext("create temporary SSTable");

  internal::SSTableBuilder builder(std::move(file.value()),
                                   options_.sstable_block_bytes);
  for (auto& e : memtable_.Scan({}, {})) {
    auto s = builder.Add(e);
    if (!s.ok())
      return s.WithContext("build temporary SSTable");
  }

  auto built = builder.Finish();
  if (!built.ok())
    return built.status().WithContext("finish temporary SSTable");

  auto verify_file = fs_->OpenRandomAccess(temp);
  if (!verify_file.ok())
    return verify_file.status().WithContext("open temporary SSTable for validation");
  auto verified = internal::SSTableReader::Open(std::move(verify_file.value()));
  if (!verified.ok())
    return verified.status().WithContext("validate temporary SSTable");

  auto s = fs_->Rename(temp, final);
  if (!s.ok())
    return s.WithContext("publish SSTable filename");
  s = fs_->SyncDir(*path_);
  if (!s.ok())
    return s.WithContext("sync database directory after SSTable rename");

  auto new_wal_file = fs_->OpenWritable(*path_ / WalName(wal_number), false);
  if (!new_wal_file.ok())
    return new_wal_file.status().WithContext("create replacement WAL");
  s = new_wal_file.value()->Sync();
  if (!s.ok())
    return s.WithContext("sync replacement WAL");
  auto new_wal = std::make_unique<internal::WalWriter>(std::move(new_wal_file.value()),
                                                       Limits(options_));

  internal::ManifestSnapshot next;
  next.active_wal_number = wal_number;
  next.next_file_number = wal_number + 1;
  next.last_sequence = next_sequence_ - 1;
  next.live_table = internal::TableMeta{table_number,
                                        built.value().file_size,
                                        built.value().smallest_key,
                                        built.value().largest_key,
                                        built.value().min_sequence,
                                        built.value().max_sequence};

  // This is the flush commit point. Before it succeeds, the old Manifest, WAL,
  // and MemTable remain authoritative even if orphan files were created.
  auto published = manifest_->Publish(next);
  if (!published.durable()) {
    if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
      terminal_error_ = published.status().WithContext(
          "publish flush MANIFEST: replacement may be visible; close and reopen "
          "the database");
      return *terminal_error_;
    }
    return published.status().WithContext("publish flush MANIFEST");
  }

  // Everything below is an in-memory ownership switch or best-effort cleanup;
  // the newly published state must remain successful once committed.
  auto old_wal = std::move(wal_);
  wal_ = std::move(new_wal);
  table_ = std::move(verified.value());
  memtable_.Clear();
  if (old_wal)
    old_wal->Close().IgnoreError();
  fs_->Remove(*path_ / WalName(current.active_wal_number)).IgnoreError();
  return Status::Ok();
}

Result<std::string> DB::Impl::Get(std::string_view key) const {
  auto s = CheckOpen();
  if (!s.ok())
    return s;

  auto mem = memtable_.Get(key);
  if (mem.ok())
    return mem.value().type == internal::ValueType::kTombstone
               ? Result<std::string>(Status::NotFound("key was deleted"))
               : Result<std::string>(mem.value().value);
  if (mem.status().code() != StatusCode::kNotFound)
    return mem.status();

  if (table_) {
    auto disk = table_->Get(key);
    if (!disk.ok())
      return disk.status();
    if (disk.value().type == internal::ValueType::kTombstone)
      return Status::NotFound("key was deleted");
    return disk.value().value;
  }

  return Status::NotFound("key is absent");
}
Result<std::vector<Entry>> DB::Impl::Scan(std::string_view begin,
                                          std::string_view end) const {
  auto s = CheckOpen();
  if (!s.ok())
    return s;
  if (!end.empty() && begin > end)
    return Status::InvalidArgument("scan begin is greater than end");

  std::map<std::string, internal::InternalEntry, internal::BytewiseLess> merged;

  if (table_) {
    auto disk = table_->Scan(begin, end);
    if (!disk.ok())
      return disk.status();
    for (auto& e : disk.value())
      merged[e.user_key] = e;
  }

  for (auto& e : memtable_.Scan(begin, end))
    merged[e.user_key] = e;

  std::vector<Entry> out;
  for (auto& [key, e] : merged)
    if (e.type == internal::ValueType::kValue)
      out.push_back({key, e.value});
  return out;
}
Status DB::Impl::Close() {
  if (closed_)
    return Status::AlreadyClosed("database is closed");

  if (!wal_) {
    closed_ = true;
    return Status::Ok();
  }

  if (options_.sync_on_write) {
    auto s = wal_->Sync();
    if (!s.ok())
      return s.WithContext("close database: sync WAL");
  }

  auto s = wal_->Close();
  closed_ = true;
  return s.ok() ? s : s.WithContext("close database: close WAL");
}
DB::Impl::~Impl() {
  if (!closed_ && wal_)
    wal_->Close().IgnoreError();
}
} // namespace tinylsm
