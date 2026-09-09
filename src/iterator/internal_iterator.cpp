#include "iterator/internal_iterator.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>

#include "util/bytewise_less.h"

namespace tinylsm::internal {
namespace {

bool InternalLess(const InternalEntry& left, const InternalEntry& right) {
  const BytewiseLess less;
  if (less(left.user_key, right.user_key))
    return true;
  if (less(right.user_key, left.user_key))
    return false;
  return left.sequence > right.sequence;
}

class VectorIterator final : public InternalIterator {
public:
  VectorIterator(std::vector<InternalEntry> entries, std::string_view begin,
                 std::string_view end)
      : entries_(std::move(entries)), end_(end) {
    Seek(begin).IgnoreError();
  }

  [[nodiscard]] bool Valid() const noexcept override {
    return status_.ok() && index_ < entries_.size() &&
           (end_.empty() || BytewiseLess{}(entries_[index_].user_key, end_));
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return entries_[index_];
  }

  Status Seek(std::string_view target) override {
    index_ = static_cast<std::size_t>(
        std::lower_bound(entries_.begin(), entries_.end(), target,
                         [](const InternalEntry& entry, std::string_view key) {
                           return BytewiseLess{}(entry.user_key, key);
                         }) -
        entries_.begin());
    return Status::Ok();
  }

  Status Next() override {
    if (Valid())
      ++index_;
    return Status::Ok();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  std::vector<InternalEntry> entries_;
  std::string end_;
  std::size_t index_ = 0;
  Status status_;
};

class MergingIterator final : public InternalIterator {
public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs)
      : inputs_(std::move(inputs)) {}

  Status Initialize() { return Seek({}); }

  [[nodiscard]] bool Valid() const noexcept override {
    return status_.ok() && winner_ != kNoWinner;
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return inputs_[winner_]->entry();
  }

  Status Seek(std::string_view target) override {
    if (!status_.ok())
      return status_;
    heap_.clear();
    winner_ = kNoWinner;
    previous_key_.clear();
    previous_sequence_ = 0;
    has_previous_ = false;
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
      auto status = inputs_[i]->Seek(target);
      if (!status.ok() || !inputs_[i]->status().ok())
        return SetError(!status.ok() ? std::move(status) : inputs_[i]->status());
      if (inputs_[i]->Valid())
        heap_.push_back(i);
    }
    std::make_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
    return SelectCurrent();
  }

  Status Next() override {
    if (!status_.ok())
      return status_;
    if (winner_ == kNoWinner)
      return Status::Ok();
    const auto prior = winner_;
    winner_ = kNoWinner;
    auto next = inputs_[prior]->Next();
    if (!next.ok() || !inputs_[prior]->status().ok())
      return SetError(!next.ok() ? std::move(next) : inputs_[prior]->status());
    if (inputs_[prior]->Valid()) {
      heap_.push_back(prior);
      std::push_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
    }
    return SelectCurrent();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  static constexpr std::size_t kNoWinner = static_cast<std::size_t>(-1);
  struct HeapCompare {
    const std::vector<std::unique_ptr<InternalIterator>>* inputs;
    bool operator()(std::size_t left, std::size_t right) const {
      return InternalLess((*inputs)[right]->entry(), (*inputs)[left]->entry());
    }
  };

  Status SelectCurrent() {
    if (heap_.empty())
      return Status::Ok();
    std::pop_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
    winner_ = heap_.back();
    heap_.pop_back();
    const auto& selected = inputs_[winner_]->entry();
    for (const auto index : heap_) {
      const auto& other = inputs_[index]->entry();
      if (other.user_key == selected.user_key && other.sequence == selected.sequence)
        return SetError(Status::Corruption("duplicate user key and sequence"));
    }
    if (has_previous_ && selected.user_key == previous_key_ &&
        selected.sequence == previous_sequence_) {
      return SetError(Status::Corruption("duplicate user key and sequence"));
    }
    previous_key_ = selected.user_key;
    previous_sequence_ = selected.sequence;
    has_previous_ = true;
    return Status::Ok();
  }

  Status SetError(Status status) {
    status_ = std::move(status);
    heap_.clear();
    winner_ = kNoWinner;
    return status_;
  }

  std::vector<std::unique_ptr<InternalIterator>> inputs_;
  std::vector<std::size_t> heap_;
  std::size_t winner_ = kNoWinner;
  std::string previous_key_;
  std::uint64_t previous_sequence_ = 0;
  bool has_previous_ = false;
  Status status_;
};

class VisibilityIterator final : public InternalIterator {
public:
  VisibilityIterator(std::unique_ptr<InternalIterator> input, std::uint64_t sequence)
      : input_(std::move(input)), sequence_(sequence) {}

  Status Initialize() { return AdvanceToVisible(); }
  [[nodiscard]] bool Valid() const noexcept override { return status_.ok() && valid_; }
  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return input_->entry();
  }

  Status Seek(std::string_view target) override {
    if (!status_.ok())
      return status_;
    valid_ = false;
    auto status = input_->Seek(target);
    if (!status.ok() || !input_->status().ok())
      return SetError(!status.ok() ? std::move(status) : input_->status());
    return AdvanceToVisible();
  }

  Status Next() override {
    if (!status_.ok())
      return status_;
    if (!valid_)
      return Status::Ok();
    const std::string previous_key = input_->entry().user_key;
    valid_ = false;
    while (input_->Valid() && input_->entry().user_key == previous_key) {
      auto status = input_->Next();
      if (!status.ok() || !input_->status().ok())
        return SetError(!status.ok() ? std::move(status) : input_->status());
    }
    return AdvanceToVisible();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  Status AdvanceToVisible() {
    while (input_->Valid()) {
      const std::string user_key = input_->entry().user_key;
      bool has_visible_version = false;
      while (input_->Valid() && input_->entry().user_key == user_key) {
        const auto& candidate = input_->entry();
        if (!has_visible_version && candidate.sequence <= sequence_) {
          has_visible_version = true;
          if (candidate.type == ValueType::kValue) {
            valid_ = true;
            return Status::Ok();
          }
        }
        auto status = input_->Next();
        if (!status.ok() || !input_->status().ok())
          return SetError(!status.ok() ? std::move(status) : input_->status());
      }
    }
    return Status::Ok();
  }

  Status SetError(Status status) {
    status_ = std::move(status);
    valid_ = false;
    return status_;
  }

  std::unique_ptr<InternalIterator> input_;
  std::uint64_t sequence_ = 0;
  bool valid_ = false;
  Status status_;
};

} // namespace

Result<std::unique_ptr<InternalIterator>>
NewVectorIterator(std::vector<InternalEntry> entries, std::string_view begin,
                  std::string_view end) {
  if (!std::is_sorted(entries.begin(), entries.end(), InternalLess))
    return Status::Corruption("captured entries are not internally ordered");
  return std::unique_ptr<InternalIterator>(
      new VectorIterator(std::move(entries), begin, end));
}

Result<std::unique_ptr<InternalIterator>>
NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs) {
  auto iterator = std::make_unique<MergingIterator>(std::move(inputs));
  auto status = iterator->Initialize();
  if (!status.ok())
    return status;
  return std::unique_ptr<InternalIterator>(std::move(iterator));
}

Result<std::unique_ptr<InternalIterator>>
NewVisibilityIterator(std::unique_ptr<InternalIterator> input, std::uint64_t sequence) {
  if (!input)
    return Status::InvalidArgument("visibility iterator input is null");
  auto iterator = std::make_unique<VisibilityIterator>(std::move(input), sequence);
  auto status = iterator->Initialize();
  if (!status.ok())
    return status;
  return std::unique_ptr<InternalIterator>(std::move(iterator));
}

} // namespace tinylsm::internal
