#include "action_encoder.h"
#include "mcts_concurrent_tree.h"
#include "mcts_game_adapter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

template <typename Function> bool throws_logic_error(Function &&function) {
  try {
    std::forward<Function>(function)();
  } catch (const std::logic_error &) {
    return true;
  }
  return false;
}

TreeKey exact_key(uint64_t position_hash) {
  return {position_hash, MCTS_TREE_KEY_VERSION, MCTS_NO_OBSERVER,
          TreeDomain::Exact, 0};
}

mcts_parallel::Policy full_policy() {
  mcts_parallel::Policy policy{};
  for (size_t action = 0; action < MAX_ACTIONS; ++action)
    policy[action] = static_cast<float>(action + 1);
  return policy;
}

bool action_encoder_reference_equivalence() {
  uint64_t random = 0x93d765dd1c82a4b7ULL;
  size_t checked_states = 0;
  size_t checked_actions = 0;
  for (uint64_t seed = 0; seed < 32; ++seed) {
    Game game(seed);
    game.simple_payment_mode = (seed & 1U) != 0;
    for (int ply = 0; ply < 72; ++ply) {
      const auto reference =
          ActionEncoderCpp::get_action_mask_reference_for_testing(game);
      const auto direct = ActionEncoderCpp::get_action_mask(game);
      if (reference != direct ||
          mcts_action_mask::from_dense(direct) !=
              ActionEncoderCpp::get_action_mask_bits(game) ||
          mcts_action_mask::from_dense(direct) !=
              ActionEncoderCpp::get_action_mask_bits_trusted(game))
        return false;

      for (int action = 0; action < ActionEncoderCpp::BASE_ACTION_COUNT;
           ++action) {
        const Action old_action =
            ActionEncoderCpp::decode_reference_for_testing(action, game);
        const Action new_action = ActionEncoderCpp::decode(action, game);
        if (old_action.pack() != new_action.pack())
          return false;
        if (!reference[static_cast<size_t>(action)])
          continue;
        const Action trusted_action =
            ActionEncoderCpp::decode_trusted(action, game);
        if (old_action.pack() != trusted_action.pack() ||
            ActionEncoderCpp::encode(new_action, game) != action ||
            !game.is_legal(new_action))
          return false;
        ++checked_actions;
      }
      for (uint64_t code : game.legal_action_codes()) {
        const Action legal = Action::unpack(code);
        const int action = ActionEncoderCpp::encode(legal, game);
        if (action >= 0 && !direct[static_cast<size_t>(action)])
          return false;
      }
      ++checked_states;

      if (game.is_game_over())
        break;
      if (game.requires_forced_pass()) {
        if (!game.apply_forced_pass(false))
          return false;
        continue;
      }
      random ^= random << 7;
      random ^= random >> 9;
      random ^= random << 8;
      if (!game.apply_random_action(random, false))
        break;
    }
  }

  // Public editor states that can hit the 2048-action cap deliberately use the
  // preserved reference path. This guards the compatibility fallback itself.
  Game capped(42);
  auto &player = capped.board.players[capped.board.current_player];
  player.gems = {3, 3, 3, 3, 3, 3};
  player.sync_packed_gems();
  if (ActionEncoderCpp::get_action_mask(capped) !=
      ActionEncoderCpp::get_action_mask_reference_for_testing(capped))
    return false;

  Game duplicate(7);
  duplicate.board.visible[0][1] = duplicate.board.visible[0][0];
  if (ActionEncoderCpp::get_action_mask(duplicate) !=
      ActionEncoderCpp::get_action_mask_reference_for_testing(duplicate))
    return false;

  return checked_states > 1000 && checked_actions > 5000;
}

bool action_bitset_equivalence() {
  for (size_t offset : {size_t{0}, size_t{16}, size_t{32}}) {
    for (uint32_t pattern = 0; pattern < (uint32_t{1} << 16); ++pattern) {
      const ActionMaskBits bits = static_cast<ActionMaskBits>(pattern)
                                  << offset;
      const DenseActionMask dense = mcts_action_mask::to_dense(bits);
      if (mcts_action_mask::from_dense(dense) != bits)
        return false;
      size_t dense_count = 0;
      uint64_t dense_digest = 0;
      for (size_t action = 0; action < MAX_ACTIONS; ++action) {
        if (dense[action]) {
          ++dense_count;
          dense_digest = dense_digest * 53 + action + 1;
        }
      }
      size_t bit_count = 0;
      uint64_t bit_digest = 0;
      mcts_action_mask::for_each(bits, [&](size_t action) {
        ++bit_count;
        bit_digest = bit_digest * 53 + action + 1;
      });
      if (dense_count != mcts_action_mask::popcount(bits) ||
          dense_count != bit_count || dense_digest != bit_digest)
        return false;
    }
  }

  for (size_t left = 0; left < MAX_ACTIONS; ++left) {
    for (size_t right = 0; right < MAX_ACTIONS; ++right) {
      const ActionMaskBits bits =
          mcts_action_mask::bit(left) | mcts_action_mask::bit(right);
      if (!mcts_action_mask::contains(bits, left) ||
          !mcts_action_mask::contains(bits, right) ||
          mcts_action_mask::from_dense(mcts_action_mask::to_dense(bits)) !=
              bits)
        return false;
    }
  }
  return true;
}

bool quiescence_fast_full_equivalence() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 8;
  ConcurrentTree tree(generation, TreeBackend::Sharded, 1024, 8);
  const auto node = tree.find_or_create(exact_key(100));
  const ActionMaskBits mask =
      mcts_action_mask::bit(3) | mcts_action_mask::bit(17);
  tree.expand_bits(node, full_policy(), Value{0.2, -0.2}, mask);
  tree.validate_quiescent_fast();
  tree.validate_quiescent_full();

  SelectionContext context;
  context.tree_generation = generation;
  auto reservation =
      tree.select_and_reserve_bits(node, mask, context, 0, nullptr);
  if (!reservation ||
      !throws_logic_error([&] { tree.validate_quiescent_fast(); }) ||
      !throws_logic_error([&] { tree.validate_quiescent_full(); }))
    return false;
  reservation->abort();
  tree.validate_quiescent_fast();
  tree.validate_quiescent_full();

  std::array<float, FEATURE_SIZE> features{};
  const auto pending_node = tree.find_or_create(exact_key(101));
  auto claim =
      tree.claim_or_attach_bits(pending_node, 1, 2, features, mask, nullptr);
  if (claim.kind != ExpansionClaimKind::Owner ||
      !throws_logic_error([&] { tree.validate_quiescent_fast(); }) ||
      !throws_logic_error([&] { tree.validate_quiescent_full(); }))
    return false;
  tree.fail_evaluation(pending_node, claim.pending, PendingState::Failed);
  tree.validate_quiescent_fast();
  tree.validate_quiescent_full();

  auto published =
      tree.claim_or_attach_bits(pending_node, 2, 2, features, mask, nullptr);
  if (published.kind != ExpansionClaimKind::Owner)
    return false;
  (void)tree.publish_evaluation(pending_node, published.pending, full_policy(),
                                Value{0.1, -0.1}, nullptr);
  tree.validate_quiescent_fast();
  tree.validate_quiescent_full();

  // The O(1) check is intentionally a quiescence check. The retained full
  // audit still catches unrelated statistic corruption in test/debug paths.
  ConcurrentTree corrupt(generation);
  const auto corrupt_node = corrupt.find_or_create(exact_key(102));
  corrupt.expand_bits(corrupt_node, full_policy(), Value{}, mask);
  corrupt.set_selection_counters_for_testing(corrupt_node, 3, 1, 0, 0, 0);
  corrupt.validate_quiescent_fast();
  return throws_logic_error([&] { corrupt.validate_quiescent_full(); });
}

struct DenseReference {
  mcts_parallel::Policy policy{};
  std::array<uint64_t, MAX_ACTIONS> visits{};
  std::array<uint64_t, MAX_ACTIONS> availability{};
  std::array<double, MAX_ACTIONS> q{};
  ActionMaskBits information_union = 0;
  uint64_t total_visits = 0;

  int select(ActionMaskBits world_mask,
             const mcts_parallel::SelectionContext &c) {
    world_mask &= mcts_action_mask::ALL;
    const size_t candidates = mcts_action_mask::popcount(world_mask);
    if (candidates == 0)
      return -1;
    double policy_sum = 0.0;
    mcts_action_mask::for_each(world_mask, [&](size_t action) {
      if (std::isfinite(policy[action]) && policy[action] > 0.0f)
        policy_sum += policy[action];
    });
    const double sqrt_total =
        std::sqrt(static_cast<double>(total_visits) + static_cast<double>(EPS));
    const double fpu = c.fpu == 0.0 ? 0.0 : -std::abs(c.fpu);
    double best_score = -std::numeric_limits<double>::infinity();
    int selected = -1;
    mcts_action_mask::for_each(world_mask, [&](size_t action) {
      const double raw = std::isfinite(policy[action]) && policy[action] > 0.0f
                             ? policy[action]
                             : 0.0;
      const double prior = policy_sum > static_cast<double>(EPS)
                               ? raw / policy_sum
                               : 1.0 / static_cast<double>(candidates);
      const double value = visits[action] > 0 ? q[action] : fpu;
      const double score =
          value + c.cpuct * prior * sqrt_total /
                      (1.0 + static_cast<double>(visits[action]));
      if (score > best_score) {
        best_score = score;
        selected = static_cast<int>(action);
      }
    });
    information_union |= world_mask;
    mcts_action_mask::for_each(world_mask,
                               [&](size_t action) { ++availability[action]; });
    return selected;
  }

  void commit(size_t action, double value) {
    ++visits[action];
    ++total_visits;
    q[action] += (value - q[action]) / static_cast<double>(visits[action]);
  }
};

bool compact_edge_dense_reference_equivalence() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 19;
  ConcurrentTree tree(generation, TreeBackend::Sharded, 1024, 8);
  const auto node = tree.find_or_create(exact_key(200));
  DenseReference reference;
  reference.policy = full_policy();
  reference.information_union = mcts_action_mask::bit(0);
  tree.expand_bits(node, reference.policy, Value{0.25, -0.25},
                   reference.information_union);

  SelectionContext context;
  context.cpuct = 1.35;
  context.fpu = 0.2;
  context.tree_generation = generation;

  for (size_t step = 0; step < MAX_ACTIONS * 4; ++step) {
    const size_t primary = step % MAX_ACTIONS;
    ActionMaskBits world = mcts_action_mask::bit(primary) |
                           mcts_action_mask::bit((primary + 7) % MAX_ACTIONS) |
                           mcts_action_mask::bit((primary + 19) % MAX_ACTIONS);
    const int expected = reference.select(world, context);
    auto reservation = tree.select_and_reserve_bits(
        node, world, context, step % NUM_PLAYERS, nullptr);
    if (!reservation || reservation->action() != expected)
      return false;
    if (step % 5 == 0) {
      reservation->abort();
    } else {
      const double value = static_cast<double>((step * 17) % 23) / 11.0 - 1.0;
      reservation->commit(value, generation);
      reference.commit(static_cast<size_t>(expected), value);
    }

    const auto snapshot = tree.snapshot(node);
    if (mcts_action_mask::from_dense(snapshot.valid_actions) !=
            reference.information_union ||
        snapshot.stats.N != reference.visits ||
        snapshot.stats.total_visits != reference.total_visits ||
        snapshot.availability_count != reference.availability ||
        snapshot.live_reservation_count != 0 ||
        snapshot.has_pending_evaluation ||
        tree.compact_edge_count_for_testing(node) !=
            mcts_action_mask::popcount(reference.information_union))
      return false;
    for (size_t action = 0; action < MAX_ACTIONS; ++action) {
      if (std::abs(snapshot.stats.Q[action] - reference.q[action]) > 1e-12)
        return false;
    }
  }

  tree.validate_quiescent_fast();
  tree.validate_quiescent_full();
  return reference.information_union == mcts_action_mask::ALL &&
         sizeof(mcts_parallel::detail::NodeRecord) <
             sizeof(mcts_parallel::MCTSNodeSnapshot64);
}

} // namespace

int main() {
  const std::array<std::pair<const char *, bool (*)()>, 4> tests = {{
      {"action encoder", action_encoder_reference_equivalence},
      {"action bitset", action_bitset_equivalence},
      {"quiescence", quiescence_fast_full_equivalence},
      {"compact edge", compact_edge_dense_reference_equivalence},
  }};
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "optimization equivalence failed: " << name << '\n';
      return 1;
    }
  }
  return 0;
}
