#include "tinylsm/db.h"

#include "db/db_impl.h"

namespace tinylsm {
DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;
DB::DB(DB&&) noexcept = default;
DB& DB::operator=(DB&&) noexcept = default;
Result<std::unique_ptr<DB>> DB::Open(const std::filesystem::path& p, Options o) {
  auto impl = Impl::Open(p, std::move(o));
  if (!impl.ok())
    return impl.status();
  return std::unique_ptr<DB>(new DB(std::move(impl.value())));
}
Status DB::Put(std::string_view k, std::string_view v) { return impl_->Put(k, v); }
Result<std::string> DB::Get(std::string_view k) const { return impl_->Get(k); }
Status DB::Delete(std::string_view k) { return impl_->Delete(k); }
Status DB::Write(const WriteBatch& batch) { return impl_->Write(batch); }
Result<std::vector<Entry>> DB::Scan(std::string_view b, std::string_view e) const {
  return impl_->Scan(b, e);
}
ReadMetrics DB::GetReadMetrics() const noexcept {
  return impl_ ? impl_->GetReadMetrics() : ReadMetrics{};
}
Status DB::Compact() { return impl_->Compact(); }
Status DB::Close() { return impl_->Close(); }
} // namespace tinylsm
