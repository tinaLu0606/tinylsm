#include "lab/storage_inspector.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

#include "db/filename.h"
#include "manifest/manifest_codec.h"
#include "sstable/sstable_format.h"
#include "util/coding.h"
#include "wal/wal_record_codec.h"

namespace tinylsm::lab {
namespace {

Result<std::string> ReadFileRange(const std::filesystem::path& path,
                                  std::uint64_t offset, std::size_t length) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error)
    return Status::IOError("inspect file size: " + error.message());
  if (offset > size || length > size - offset)
    return Status::InvalidArgument("requested byte range is outside the file");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return Status::IOError("open inspection file failed");
  input.seekg(static_cast<std::streamoff>(offset));
  std::string out(length, '\0');
  input.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (input.gcount() != static_cast<std::streamsize>(out.size()))
    return Status::IOError("read inspection file failed");
  return out;
}

InspectedEntry CopyEntry(const internal::InternalEntry& entry) {
  return {entry.user_key, entry.value, entry.sequence,
          entry.type == internal::ValueType::kTombstone};
}

} // namespace

Result<std::filesystem::path>
StorageInspector::CanonicalFile(std::string_view name, bool allow_manifest) const {
  if (name.empty() || std::filesystem::path(name).filename() != name)
    return Status::InvalidArgument("storage filename is not canonical");
  const auto candidate = database_path_ / std::string(name);
  if (!(allow_manifest && name == "MANIFEST")) {
    const auto numbered = internal::ParseNumberedFileName(name);
    if (!numbered)
      return Status::InvalidArgument("storage filename is not a TinyLSM file");
  }
  std::error_code error;
  if (std::filesystem::is_symlink(candidate, error))
    return Status::InvalidArgument("storage inspection does not follow symbolic links");
  if (error)
    return Status::IOError("inspect storage path: " + error.message());
  return candidate;
}

Result<ManifestInspection> StorageInspector::Manifest() const {
  auto path = CanonicalFile("MANIFEST", true);
  if (!path.ok())
    return path.status();
  std::error_code error;
  const auto size = std::filesystem::file_size(path.value(), error);
  if (error)
    return Status::IOError("inspect MANIFEST size: " + error.message());
  if (size > internal::kMaxManifestFileBytes)
    return Status::ResourceExhausted("MANIFEST exceeds inspection limit");
  auto bytes = ReadFileRange(path.value(), 0, static_cast<std::size_t>(size));
  if (!bytes.ok())
    return bytes.status();
  auto decoded = internal::ManifestCodec::Decode(internal::AsBytes(bytes.value()));
  if (!decoded.ok())
    return decoded.status();
  std::uint16_t version = 0;
  internal::GetFixed16(internal::AsBytes(bytes.value()), 4, version);
  return ManifestInspection{version, decoded.value().active_wal_number,
                            decoded.value().next_file_number,
                            decoded.value().last_sequence};
}

Result<PagedResult<WalRecordPageItem>>
StorageInspector::WalRecords(std::string_view name, std::uint64_t cursor,
                             std::size_t limit) const {
  auto path = CanonicalFile(name, false);
  if (!path.ok())
    return path.status();
  const auto numbered = internal::ParseNumberedFileName(name);
  if (!numbered || numbered->type != internal::NumberedFileType::kWal)
    return Status::InvalidArgument("requested file is not a WAL");
  if (limit == 0 || limit > kMaxPageItems)
    return Status::InvalidArgument("WAL page limit is outside the server bound");

  std::error_code error;
  const auto size = std::filesystem::file_size(path.value(), error);
  if (error)
    return Status::IOError("inspect WAL size: " + error.message());
  if (cursor > size)
    return Status::InvalidArgument("WAL cursor is outside the file");

  PagedResult<WalRecordPageItem> page;
  std::uint64_t offset = cursor;
  while (offset < size && page.items.size() < limit) {
    const auto header = ReadFileRange(path.value(), offset, internal::kWalHeaderSize);
    if (!header.ok()) {
      page.items.push_back({offset, 0, std::nullopt, header.status()});
      break;
    }
    std::uint32_t payload_size = 0;
    internal::GetFixed32(internal::AsBytes(header.value()), 8, payload_size);
    if (payload_size > size - offset - internal::kWalHeaderSize) {
      page.items.push_back(
          {offset, size - offset, std::nullopt,
           Status::Corruption("WAL payload length extends beyond the file")});
      offset = size;
      break;
    }
    const auto encoded =
        internal::kWalHeaderSize + static_cast<std::uint64_t>(payload_size);
    auto record =
        ReadFileRange(path.value(), offset, static_cast<std::size_t>(encoded));
    WalRecordPageItem item{.offset = offset, .encoded_bytes = encoded};
    if (!record.ok()) {
      item.validation_error = record.status();
    } else {
      auto decoded = internal::DecodeWalRecord(internal::AsBytes(record.value()), {});
      if (decoded.ok())
        item.entry = CopyEntry(decoded.value());
      else {
        auto batch = internal::DecodeWalBatch(internal::AsBytes(record.value()), {});
        if (!batch.ok())
          item.validation_error = batch.status();
        else if (batch.value().empty())
          item.validation_error = Status::Corruption("WAL batch is empty");
        else
          item.entry = CopyEntry(batch.value().front());
      }
    }
    page.items.push_back(std::move(item));
    offset += encoded;
  }
  page.has_more = offset < size;
  if (page.has_more)
    page.next_cursor = offset;
  return page;
}

Result<PagedResult<SstableBlockPageItem>>
StorageInspector::SstableBlocks(std::string_view name, std::uint64_t cursor,
                                std::size_t limit) const {
  auto path = CanonicalFile(name, false);
  if (!path.ok())
    return path.status();
  const auto numbered = internal::ParseNumberedFileName(name);
  if (!numbered || numbered->type != internal::NumberedFileType::kSstable)
    return Status::InvalidArgument("requested file is not an SSTable");
  if (limit == 0 || limit > kMaxPageItems)
    return Status::InvalidArgument("SSTable page limit is outside the server bound");

  std::error_code error;
  const auto size = std::filesystem::file_size(path.value(), error);
  if (error)
    return Status::IOError("inspect SSTable size: " + error.message());
  if (size < internal::kSstableV1FooterSize)
    return Status::Corruption("SSTable is smaller than its footer");
  const auto v1_footer_bytes =
      ReadFileRange(path.value(), size - internal::kSstableV1FooterSize,
                    internal::kSstableV1FooterSize);
  if (!v1_footer_bytes.ok())
    return v1_footer_bytes.status();
  auto footer = internal::DecodeFooter(internal::AsBytes(v1_footer_bytes.value()));
  if (!footer.ok()) {
    if (size < internal::kSstableFooterSize)
      return footer.status();
    const auto v2_footer_bytes =
        ReadFileRange(path.value(), size - internal::kSstableFooterSize,
                      internal::kSstableFooterSize);
    if (!v2_footer_bytes.ok())
      return v2_footer_bytes.status();
    footer = internal::DecodeFooter(internal::AsBytes(v2_footer_bytes.value()));
    if (!footer.ok())
      return footer.status();
  }
  const std::uint64_t footer_size =
      footer.value().version == internal::kSstableVersionV1
          ? internal::kSstableV1FooterSize
          : internal::kSstableFooterSize;
  const std::uint64_t sections_end = size - footer_size;
  if (footer.value().index_offset > sections_end ||
      footer.value().index_size > sections_end - footer.value().index_offset ||
      footer.value().index_size > kMaxRangeBytes ||
      (footer.value().version == internal::kSstableVersionV1 &&
       footer.value().index_offset + footer.value().index_size != sections_end) ||
      (footer.value().version == internal::kSstableVersion &&
       (footer.value().index_offset + footer.value().index_size !=
            footer.value().properties_offset ||
        footer.value().properties_offset > sections_end ||
        footer.value().properties_size >
            sections_end - footer.value().properties_offset ||
        footer.value().properties_offset + footer.value().properties_size !=
            sections_end))) {
    return Status::Corruption("SSTable index range is invalid or too large");
  }
  auto index_bytes = ReadFileRange(path.value(), footer.value().index_offset,
                                   static_cast<std::size_t>(footer.value().index_size));
  if (!index_bytes.ok())
    return index_bytes.status();
  auto blocks = internal::DecodeIndex(internal::AsBytes(index_bytes.value()));
  if (!blocks.ok())
    return blocks.status();
  if (cursor > blocks.value().size())
    return Status::InvalidArgument("SSTable cursor is outside the block index");

  PagedResult<SstableBlockPageItem> page;
  const auto end = std::min<std::size_t>(blocks.value().size(), cursor + limit);
  for (std::size_t index = static_cast<std::size_t>(cursor); index < end; ++index) {
    const auto& meta = blocks.value()[index];
    SstableBlockPageItem item{.index = index,
                              .offset = meta.offset,
                              .size = meta.size,
                              .smallest_key = meta.first_key,
                              .largest_key = meta.last_key};
    if (meta.size > kMaxRangeBytes) {
      item.validation_error =
          Status::ResourceExhausted("SSTable block exceeds inspection limit");
    } else {
      auto block =
          ReadFileRange(path.value(), meta.offset, static_cast<std::size_t>(meta.size));
      if (!block.ok()) {
        item.validation_error = block.status();
      } else {
        auto decoded = internal::DecodeDataBlock(internal::AsBytes(block.value()),
                                                 footer.value().version);
        if (!decoded.ok()) {
          item.validation_error = decoded.status();
        } else {
          item.entries.reserve(decoded.value().size());
          for (const auto& entry : decoded.value())
            item.entries.push_back(CopyEntry(entry));
        }
      }
    }
    page.items.push_back(std::move(item));
  }
  page.has_more = end < blocks.value().size();
  if (page.has_more)
    page.next_cursor = end;
  return page;
}

Result<std::string> StorageInspector::FileBytes(std::string_view name,
                                                std::uint64_t offset,
                                                std::size_t length) const {
  if (length == 0 || length > kMaxRangeBytes)
    return Status::InvalidArgument("byte range length is outside the server bound");
  auto path = CanonicalFile(name, true);
  if (!path.ok())
    return path.status();
  return ReadFileRange(path.value(), offset, length);
}

} // namespace tinylsm::lab
