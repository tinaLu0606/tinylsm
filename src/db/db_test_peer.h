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
};

} // namespace tinylsm::internal
