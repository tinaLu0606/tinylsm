#include "iterator/internal_iterator.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

#include "util/bytewise_less.h"

namespace tinylsm::internal {
namespace {

class MergingIterator final : public InternalIterator {
public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs)
      : inputs_(std::move(inputs)) {}

  Status Initialize() {
    heap_.reserve(inputs_.size());
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
      if (!inputs_[i])
        return SetError(Status::InvalidArgument("merge iterator input is null"));
      if (!inputs_[i]->status().ok())
        return SetError(inputs_[i]->status());
      if (inputs_[i]->Valid())
        heap_.push_back(i);
    }
    std::make_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
    return SelectCurrent();
  }

  [[nodiscard]] bool Valid() const noexcept override {
    return status_.ok() && has_winner_;
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return inputs_[winner_]->entry();
  }

  Status Next() override {
    if (!status_.ok())
      return status_;
    if (!has_winner_)
      return Status::Ok();

    has_winner_ = false;
    for (const auto index : current_) {
      auto next = inputs_[index]->Next();
      if (!next.ok())
        return SetError(next);
      if (!inputs_[index]->status().ok())
        return SetError(inputs_[index]->status());
      if (inputs_[index]->Valid()) {
        heap_.push_back(index);
        std::push_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
      }
    }
    current_.clear();
    return SelectCurrent();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  struct HeapCompare {
    const std::vector<std::unique_ptr<InternalIterator>>* inputs;

    bool operator()(std::size_t left, std::size_t right) const {
      const auto& left_key = (*inputs)[left]->entry().user_key;
      const auto& right_key = (*inputs)[right]->entry().user_key;
      return BytewiseLess{}(right_key, left_key);
    }
  };

  std::size_t PopHeap() {
    std::pop_heap(heap_.begin(), heap_.end(), HeapCompare{&inputs_});
    const auto index = heap_.back();
    heap_.pop_back();
    return index;
  }

  Status SelectCurrent() {
    if (heap_.empty())
      return Status::Ok();

    const auto first = PopHeap();
    current_key_ = inputs_[first]->entry().user_key;
    current_.push_back(first);
    while (!heap_.empty() && inputs_[heap_.front()]->entry().user_key == current_key_) {
      current_.push_back(PopHeap());
    }

    winner_ = first;
    has_winner_ = true;
    for (std::size_t i = 0; i < current_.size(); ++i) {
      const auto sequence = inputs_[current_[i]]->entry().sequence;
      for (std::size_t j = 0; j < i; ++j) {
        if (inputs_[current_[j]]->entry().sequence == sequence) {
          return SetError(Status::Corruption("duplicate key has the same sequence"));
        }
      }
      if (inputs_[winner_]->entry().sequence < sequence)
        winner_ = current_[i];
    }
    return Status::Ok();
  }

  Status SetError(Status status) {
    status_ = std::move(status);
    has_winner_ = false;
    current_.clear();
    heap_.clear();
    return status_;
  }

  std::vector<std::unique_ptr<InternalIterator>> inputs_;
  std::vector<std::size_t> heap_;
  std::deque<std::size_t> current_;
  std::size_t winner_ = 0;
  bool has_winner_ = false;
  std::string current_key_;
  Status status_;
};

} // namespace

Result<std::unique_ptr<InternalIterator>>
NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs) {
  auto iterator = std::make_unique<MergingIterator>(std::move(inputs));
  auto status = iterator->Initialize();
  if (!status.ok())
    return status;
  return std::unique_ptr<InternalIterator>(std::move(iterator));
}

} // namespace tinylsm::internal
