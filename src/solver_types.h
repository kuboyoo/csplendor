#ifndef CSPLENDOR_SOLVER_TYPES_H
#define CSPLENDOR_SOLVER_TYPES_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <memory>
#include <vector>

struct VisibleOnlySearchStats {
  uint64_t nodes = 0;
  uint64_t memo_hits = 0;
  uint64_t terminal_nodes = 0;
  uint64_t legal_moves = 0;
  double elapsed_ms = 0.0;
};

struct VisibleOnlyLineEntry {
  uint64_t action_code = 0;
  int winner = -1;
  std::string reason;
  size_t action_count = 0;
};

struct VisibleOnlySearchResult {
  int winner = -1;
  int forced_win_depth = -1;
  bool simple_payment_mode = false;
  std::string winner_reason = "unknown";
  std::string unknown_reason;
  size_t memoized_states = 0;
  VisibleOnlySearchStats stats;
  std::vector<VisibleOnlyLineEntry> line;
};

struct RevealVerifiedSearchStats {
  uint64_t nodes = 0;
  uint64_t memo_hits = 0;
  uint64_t persistent_memo_hits = 0;
  uint64_t iterative_order_hits = 0;
  uint64_t terminal_nodes = 0;
  uint64_t legal_moves = 0;
  uint64_t reveal_branches = 0;
  uint64_t final_round_reveal_collapses = 0;
  uint64_t final_round_score_prunes = 0;
  uint64_t final_round_direct_resolutions = 0;
  uint64_t oracle_purchase_actions = 0;
  uint64_t oracle_reserve_actions = 0;
  uint64_t deck_reserve_candidates = 0;
  uint64_t deck_reserve_branches = 0;
  double elapsed_ms = 0.0;
};

struct RevealVerifiedLineEntry {
  uint64_t action_code = 0;
  int reveal_card = -1;
  size_t action_count = 0;
};

struct RevealVerifiedProofEdge {
  uint64_t action_code = 0;
  int reveal_card = -1;
  int oracle_card = -1;
  bool oracle_reserve = false;
  int oracle_reserve_card = -1;
  int oracle_return_color = -1;
  std::array<uint8_t, 5> oracle_gold_as = {0};
  size_t child = 0;
};

struct RevealVerifiedProofNode {
  size_t id = 0;
  int player = -1;
  int depth = 0;
  std::array<int, 2> scores = {0, 0};
  int winner = -1;
  bool waiting_noble = false;
  std::vector<int> nobles;
  std::array<std::vector<int>, 2> acquired_nobles;
  std::string kind = "state";
  std::string resolution;
  std::vector<RevealVerifiedProofEdge> children;
};

struct RevealVerifiedProofDag {
  bool requested = false;
  bool complete = false;
  bool validated = false;
  std::string omitted_reason;
  size_t root = 0;
  std::vector<RevealVerifiedProofNode> nodes;
};

struct RevealVerifiedSearchResult {
  bool proven = false;
  int attacker = -1;
  int depth = -1;
  std::string reason = "unknown";
  std::string unknown_reason;
  size_t memoized_states = 0;
  RevealVerifiedSearchStats stats;
  std::vector<RevealVerifiedLineEntry> line;
  RevealVerifiedProofDag proof_dag;
};

namespace csplendor::solver_internal {

class RevealSearchCancellationToken {
public:
  explicit RevealSearchCancellationToken(
      std::shared_ptr<RevealSearchCancellationToken> parent = {}) noexcept
      : parent_(std::move(parent)) {}

  void request_cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
  }

  void reset() noexcept { cancelled_.store(false, std::memory_order_release); }

  bool is_cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire) ||
           (parent_ && parent_->is_cancelled());
  }

private:
  std::atomic<bool> cancelled_{false};
  std::shared_ptr<RevealSearchCancellationToken> parent_;
};

struct SearchLimitExceeded : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

class SearchLimit {
public:
  SearchLimit(
      uint64_t max_nodes, double time_limit_seconds,
      std::shared_ptr<RevealSearchCancellationToken> cancellation_token = {})
      : max_nodes_(max_nodes), time_limit_seconds_(time_limit_seconds),
        cancellation_token_(std::move(cancellation_token)) {}

  void reset() noexcept { start_time_ = Clock::now(); }

  void check(uint64_t visited_nodes) const {
    // Node limits are exact.  Clock and atomic cancellation reads are sampled:
    // at solver throughput, checking them on every node costs a meaningful
    // fraction of the available move time while adding negligible response
    // latency.  Node zero keeps immediate timeout/pre-cancellation semantics.
    constexpr uint64_t EXPENSIVE_CHECK_INTERVAL = 64;
    const bool poll_expensive_limits =
        (visited_nodes & (EXPENSIVE_CHECK_INTERVAL - 1)) == 0;
    if (poll_expensive_limits && cancellation_token_ &&
        cancellation_token_->is_cancelled())
      throw SearchLimitExceeded("search cancelled");
    if (max_nodes_ && visited_nodes >= max_nodes_)
      throw SearchLimitExceeded("node limit exceeded");
    if (poll_expensive_limits && time_limit_seconds_ > 0.0 &&
        elapsed_ms() >= time_limit_seconds_ * 1000.0)
      throw SearchLimitExceeded("time limit exceeded");
  }

  double elapsed_ms() const noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_time_)
        .count();
  }

private:
  using Clock = std::chrono::steady_clock;

  uint64_t max_nodes_ = 0;
  double time_limit_seconds_ = 0.0;
  std::shared_ptr<RevealSearchCancellationToken> cancellation_token_;
  Clock::time_point start_time_ = Clock::now();
};

enum class ForceStatus : uint8_t { PROVEN, REFUTED, UNKNOWN };

struct StateKeyCore {
  uint64_t board_hash = 0;
  uint8_t points0 = 0;
  uint8_t points1 = 0;
  uint8_t purchased0 = 0;
  uint8_t purchased1 = 0;
  bool final_round = false;
  int8_t winner = -1;

  bool operator==(const StateKeyCore &other) const noexcept {
    return board_hash == other.board_hash && points0 == other.points0 &&
           points1 == other.points1 && purchased0 == other.purchased0 &&
           purchased1 == other.purchased1 && final_round == other.final_round &&
           winner == other.winner;
  }

  uint64_t metadata_bits() const noexcept {
    uint64_t meta = static_cast<uint64_t>(points0);
    meta |= static_cast<uint64_t>(points1) << 8;
    meta |= static_cast<uint64_t>(purchased0) << 16;
    meta |= static_cast<uint64_t>(purchased1) << 24;
    meta |= static_cast<uint64_t>(final_round ? 1 : 0) << 32;
    meta |= static_cast<uint64_t>(static_cast<uint8_t>(winner + 2)) << 33;
    return meta;
  }
};

struct ActionOrderKey {
  int rank = 9;
  int neg_points = 0;
  uint64_t code = 0;

  bool operator<(const ActionOrderKey &other) const noexcept {
    if (rank != other.rank)
      return rank < other.rank;
    if (neg_points != other.neg_points)
      return neg_points < other.neg_points;
    return code < other.code;
  }
};

struct TerminalResult {
  bool terminal = false;
  int winner = -1;
  const char *reason = "unknown";
};

struct ZeroSumScore {
  static int from_winner(int winner) noexcept {
    return winner == 0 ? 1 : winner == 1 ? -1 : 0;
  }

  static int winner(int score) noexcept {
    return score > 0 ? 0 : score < 0 ? 1 : -2;
  }
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_TYPES_H
