#ifndef CSPLENDOR_SOLVER_SEARCH_SCRATCH_H
#define CSPLENDOR_SOLVER_SEARCH_SCRATCH_H

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace csplendor::solver_internal {

// Indexed by live invocation count, NOT remaining mate depth. Owning pointers
// preserves parent references even through noble choices and nested proof work.
// Frames never escape a lease; returned proofs/frontiers retain their own data.
template <typename Frame> class RecursionScratch {
public:
  RecursionScratch() = default;
  // Scratch is not search state. Copies start empty and never share storage.
  RecursionScratch(const RecursionScratch &) {}
  RecursionScratch &operator=(const RecursionScratch &) {
    assert(active_ == 0);
    frames_.clear();
    return *this;
  }

  class Lease {
  public:
    explicit Lease(RecursionScratch &owner) : owner_(owner) {
      if (owner_.active_ == owner_.frames_.size())
        owner_.frames_.push_back(std::make_unique<Frame>());
      index_ = owner_.active_++;
      frame_ = owner_.frames_[index_].get();
      frame_->clear();
    }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    ~Lease() {
      assert(owner_.active_ == index_ + 1);
      --owner_.active_;
    }
    Frame &get() const noexcept { return *frame_; }

  private:
    RecursionScratch &owner_;
    size_t index_ = 0;
    Frame *frame_ = nullptr;
  };

  size_t active() const noexcept { return active_; }
  const std::vector<std::unique_ptr<Frame>> &frames() const noexcept { return frames_; }

private:
  std::vector<std::unique_ptr<Frame>> frames_;
  size_t active_ = 0;
};

} // namespace csplendor::solver_internal

#endif
