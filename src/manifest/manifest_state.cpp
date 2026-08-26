#include "manifest/manifest_state.h"

#include <array>
#include <vector>

#include "util/coding.h"

namespace tinylsm::internal {
namespace {
constexpr const char* kManifest = "MANIFEST";
constexpr const char* kTemp = "MANIFEST.tmp";
} // namespace

Result<ManifestSnapshot> ManifestState::Load(FileSystem& fs,
                                             const std::filesystem::path& db) {
  auto file = fs.OpenRandomAccess(db / kManifest);
  if (!file.ok())
    return file.status();
  auto size = file.value()->Size();
  if (!size.ok())
    return size.status();
  if (size.value() > 128U * 1024U * 1024U)
    return Status::Corruption("manifest is too large");
  std::vector<std::byte> bytes(size.value());
  auto s = ReadExactly(*file.value(), 0, bytes);
  if (!s.ok())
    return s;
  return ManifestCodec::Decode(bytes);
}
Status ManifestState::Publish(const ManifestSnapshot& next) {
  auto encoded = ManifestCodec::Encode(next);
  if (!encoded.ok())
    return encoded.status();
  auto file = fs_.OpenWritable(db_path_ / kTemp, false);
  if (!file.ok())
    return file.status();
  auto s = file.value()->Append(AsBytes(encoded.value()));
  if (!s.ok())
    return s;
  s = file.value()->Sync();
  if (!s.ok())
    return s;
  s = file.value()->Close();
  if (!s.ok())
    return s;
  s = fs_.Rename(db_path_ / kTemp, db_path_ / kManifest);
  if (!s.ok())
    return s;
  s = fs_.SyncDir(db_path_);
  if (!s.ok())
    return s;
  current_ = next;
  return Status::Ok();
}
} // namespace tinylsm::internal
