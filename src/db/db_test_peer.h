#pragma once

#include <filesystem>
#include <memory>

#include "db/db_impl.h"
#include "io/file.h"
#include "tinylsm/db.h"

namespace tinylsm::internal {

/// Internal test seam for opening a DB with a fault-injectable FileSystem.
class DBTestPeer {
public:
  static Result<std::unique_ptr<DB>> Open(const std::filesystem::path& path,
                                          Options options,
                                          std::unique_ptr<FileSystem> fs) {
    auto impl = DB::Impl::Open(path, std::move(options), std::move(fs));
    if (!impl.ok())
      return impl.status();
    return std::unique_ptr<DB>(new DB(std::move(impl.value())));
  }

  /// Test-only synchronization point for a scheduled immutable flush. It does
  /// not create a table or mutate user data beyond allowing existing work to
  /// finish.
  static Status WaitForBackgroundFlush(DB& db) {
    std::unique_lock lock(db.impl_->mutex_);
    db.impl_->WaitForBackgroundFlush(lock);
    return db.impl_->CheckOpen();
  }

  /// Test-only synchronization point for all queued background maintenance,
  /// including a size-tiered compaction after an immutable flush.
  static Status WaitForBackgroundWork(DB& db) {
    std::unique_lock lock(db.impl_->mutex_);
    db.impl_->WaitForBackgroundWork(lock);
    return db.impl_->CheckOpen();
  }

  /// Schedules one normal worker compaction after Open for deterministic
  /// concurrency tests. Production scheduling still comes from table debt.
  static void RequestBackgroundCompaction(DB& db, std::size_t table_trigger) {
    std::unique_lock lock(db.impl_->mutex_);
    db.impl_->options_.compaction_table_trigger = table_trigger;
    db.impl_->compaction_requested_ = true;
    db.impl_->background_cv_.notify_all();
  }
};

} // namespace tinylsm::internal
