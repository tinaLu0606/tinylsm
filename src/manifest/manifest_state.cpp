#include "manifest/manifest_state.h"

#include <array>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "util/coding.h"

namespace tinylsm::internal {
namespace {
constexpr const char* kManifest = "MANIFEST";
constexpr const char* kTemp = "MANIFEST.tmp";
static_assert(std::is_nothrow_move_assignable_v<ManifestSnapshot>);
} // namespace

ManifestPublishOutcome ManifestPublishOutcome::NotPublished(Status status) {
  if (status.ok())
    throw std::invalid_argument("not-published outcome requires an error Status");
  return {ManifestPublishState::kNotPublished, std::move(status)};
}

ManifestPublishOutcome ManifestPublishOutcome::VisibleNotDurable(Status status) {
  if (status.ok())
    throw std::invalid_argument("visible-not-durable outcome requires an error Status");
  return {ManifestPublishState::kVisibleNotDurable, std::move(status)};
}

ManifestPublishOutcome ManifestPublishOutcome::Durable() {
  return {ManifestPublishState::kDurable, Status::Ok()};
}

Result<ManifestSnapshot> ManifestState::Load(FileSystem& fs,
                                             const std::filesystem::path& db) {
  auto file = fs.OpenRandomAccess(db / kManifest);
  if (!file.ok())
    return file.status();

  auto size = file.value()->Size();
  if (!size.ok())
    return size.status();
  if (size.value() > kMaxManifestFileBytes)
    return Status::Corruption("manifest is too large");

  std::vector<std::byte> bytes(size.value());
  auto s = ReadExactly(*file.value(), 0, bytes);
  if (!s.ok())
    return s;
  return ManifestCodec::Decode(bytes);
}
ManifestPublishOutcome ManifestState::Publish(ManifestSnapshot next) {
  auto encoded = ManifestCodec::Encode(next);
  if (!encoded.ok())
    return ManifestPublishOutcome::NotPublished(encoded.status());

  auto file = fs_.OpenWritable(db_path_ / kTemp, false);
  if (!file.ok())
    return ManifestPublishOutcome::NotPublished(file.status());
  auto s = file.value()->Append(AsBytes(encoded.value()));
  if (!s.ok())
    return ManifestPublishOutcome::NotPublished(std::move(s));

  s = file.value()->Sync();
  if (!s.ok())
    return ManifestPublishOutcome::NotPublished(std::move(s));
  s = file.value()->Close();
  if (!s.ok())
    return ManifestPublishOutcome::NotPublished(std::move(s));

  s = fs_.Rename(db_path_ / kTemp, db_path_ / kManifest);
  if (!s.ok())
    return ManifestPublishOutcome::NotPublished(std::move(s));
  s = fs_.SyncDir(db_path_);
  if (!s.ok())
    return ManifestPublishOutcome::VisibleNotDurable(std::move(s));

  current_ = std::move(next);
  return ManifestPublishOutcome::Durable();
}
} // namespace tinylsm::internal
