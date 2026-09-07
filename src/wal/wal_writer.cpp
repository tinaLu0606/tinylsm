#include "wal/wal_writer.h"

#include "util/coding.h"

namespace tinylsm::internal {

Status WalWriter::Append(const InternalEntry& entry) {
  auto encoded = EncodeWalRecord(entry, limits_);
  if (!encoded.ok())
    return encoded.status();
  return file_->Append(AsBytes(encoded.value()));
}

Status WalWriter::AppendBatch(std::span<const InternalEntry> entries) {
  auto encoded = EncodeWalBatch(entries, limits_);
  if (!encoded.ok())
    return encoded.status();
  return file_->Append(AsBytes(encoded.value()));
}

} // namespace tinylsm::internal
