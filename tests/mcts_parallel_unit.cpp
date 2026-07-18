#include "mcts.h"
#include "mcts_game_adapter.h"
#include "mcts_rng.h"
#include "mcts_tree_key.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static_assert(
    !std::is_copy_constructible_v<mcts_parallel::SelectionReservation>,
    "a reservation must be move-only");
static_assert(!std::is_copy_assignable_v<mcts_parallel::SelectionReservation>,
              "a reservation must be move-only");
static_assert(std::is_move_constructible_v<mcts_parallel::SelectionReservation>,
              "a reservation must be movable");
static_assert(!std::is_copy_constructible_v<mcts_parallel::ReservedPath>,
              "a reserved path must be move-only");
static_assert(std::is_move_constructible_v<mcts_parallel::ReservedPath>,
              "a reserved path must be movable");
static_assert(!std::is_copy_constructible_v<MCTS::SearchGuard>,
              "an active-search guard must be move-only");
static_assert(std::is_move_constructible_v<MCTS::SearchGuard>,
              "an active-search guard must be movable");

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

mcts_parallel::ActionMask mask_with(std::initializer_list<size_t> actions) {
  mcts_parallel::ActionMask mask{};
  for (size_t action : actions)
    mask.at(action) = 1;
  return mask;
}

mcts_parallel::Policy
policy_with(std::initializer_list<std::pair<size_t, float>> entries) {
  mcts_parallel::Policy policy{};
  for (const auto &[action, probability] : entries)
    policy.at(action) = probability;
  return policy;
}

mcts_parallel::Value test_value(double base = 0.25) {
  mcts_parallel::Value value{};
  for (size_t player = 0; player < NUM_PLAYERS; ++player)
    value[player] = base + static_cast<double>(player) * 0.1;
  return value;
}

bool test_tree_key_domains() {
  Game game(42);
  const TreeKey observable_zero =
      mcts_internal::GameAdapter::tree_key(game, 0, true);
  const TreeKey observable_one =
      mcts_internal::GameAdapter::tree_key(game, 1, true);
  const TreeKey exact = mcts_internal::GameAdapter::tree_key(game, 0, false);
  const TreeKey exact_one =
      mcts_internal::GameAdapter::tree_key(game, 1, false);
  if (observable_zero == observable_one || observable_zero == exact ||
      exact == exact_one || exact.observer != 0 || exact_one.observer != 1)
    return false;

  Game simple = game.clone_light();
  simple.simple_payment_mode = true;
  const TreeKey simple_key =
      mcts_internal::GameAdapter::tree_key(simple, 0, true);
  if (simple_key == observable_zero ||
      !(simple_key.mode_bits & MCTS_MODE_SIMPLE_PAYMENT))
    return false;

  std::unordered_map<TreeKey, int, TreeKeyHash> map;
  map.emplace(observable_zero, 1);
  map.emplace(observable_one, 2);
  map.emplace(exact, 3);
  map.emplace(simple_key, 4);
  map.emplace(exact_one, 5);
  return map.size() == 5 && make_legacy_tree_key(exact.position_hash).domain ==
                                TreeDomain::LegacyExact;
}

bool test_hidden_tier_hash() {
  Game game(42);
  PlayerState player = game.board.players[0];
  player.reserved[0] = 0;
  player.reserved_count = 1;
  player.reserved_is_hidden[0] = true;
  game.board.players[0] = player;
  game.board.invalidate_hash();
  const uint64_t level_one = game.board.observable_hash(1);

  game.board.players[0].reserved[0] = 40;
  game.board.invalidate_hash();
  const uint64_t level_two = game.board.observable_hash(1);
  game.board.players[0].reserved[0] = 41;
  game.board.invalidate_hash();
  const uint64_t same_level_two = game.board.observable_hash(1);
  return level_one != level_two && level_two == same_level_two;
}

bool test_hidden_multi_world_rejected_before_mutation() {
  MCTSConfig config;
  config.use_determinization = true;
  config.use_dirichlet_noise = false;
  MCTS mcts(config);
  Game root(42);
  try {
    (void)mcts.prepare_batch_simulations(root, 0, 1, 2, nullptr);
  } catch (const std::invalid_argument &) {
    return mcts.tree_size() == 0;
  }
  return false;
}

bool test_portable_rng_golden() {
  PortableRng stream(0);
  const std::array<uint64_t, 5> expected = {
      16294208416658607535ULL, 7960286522194355700ULL, 487617019471545679ULL,
      17909611376780542444ULL, 1961750202426094747ULL};
  for (uint64_t value : expected) {
    if (stream.next_u64() != value)
      return false;
  }

  PortableRng bounded(42);
  const std::array<uint64_t, 8> bounded_expected = {3, 1, 8, 4, 0, 2, 5, 8};
  for (uint64_t value : bounded_expected) {
    if (bounded.uniform_bounded(10) != value)
      return false;
  }
  try {
    (void)bounded.uniform_bounded(0);
    return false;
  } catch (const std::invalid_argument &) {
  }

  std::array<int, 8> values = {0, 1, 2, 3, 4, 5, 6, 7};
  PortableRng shuffle_rng(123);
  portable_shuffle(values.begin(), values.end(), shuffle_rng);
  if (values != std::array<int, 8>{6, 0, 7, 2, 1, 4, 5, 3})
    return false;

  TreeKey key{1234, MCTS_TREE_KEY_VERSION, 0, TreeDomain::Observable, 0};
  return derive_search_seed(0, key, 7, SearchRandomDomain::RootDeterminization,
                            9) == 4284427826275011102ULL &&
         derive_search_seed(0, key, 7, SearchRandomDomain::RootDirichlet, 9) ==
             17268196273200771849ULL;
}

bool test_portable_determinization_contract() {
  Game root(42);
  Action reserve;
  bool found = false;
  for (const Action &action : root.legal_actions()) {
    if (action.type == RESERVE_DECK && action.deck_level == 0) {
      reserve = action;
      found = true;
      break;
    }
  }
  if (!found || !root.apply_trusted(reserve, false))
    return false;
  const uint64_t public_key = root.board.observable_hash(1);
  Game first = root.shuffled_clone_portable(1, 123);
  Game repeated = root.shuffled_clone_portable(1, 123);
  Game different = root.shuffled_clone_portable(1, 124);
  return first.board.compute_hash_uncached() ==
             repeated.board.compute_hash_uncached() &&
         first.board.compute_hash_uncached() !=
             different.board.compute_hash_uncached() &&
         first.board.observable_hash(1) == public_key &&
         different.board.observable_hash(1) == public_key;
}

bool test_seed_manifest_is_logical_id_based() {
  SearchRandomContext context;
  context.resolved_master_seed = 0;
  context.root_key = {0x123456789abcdef0ULL, MCTS_TREE_KEY_VERSION, 1,
                      TreeDomain::Observable, MCTS_MODE_SIMPLE_PAYMENT};
  context.search_nonce = 17;
  context.simulation_id_base = 1000;

  std::array<uint64_t, 128> forward{};
  std::unordered_set<uint64_t> unique;
  for (std::size_t index = 0; index < forward.size(); ++index) {
    const uint64_t simulation_id = context.simulation_id_base + index;
    forward[index] = context.seed_for(SearchRandomDomain::RootDeterminization,
                                      simulation_id);
    unique.insert(forward[index]);
  }
  if (unique.size() != forward.size())
    return false;

  // Reverse completion/worker assignment must not change a logical ticket's
  // seed, and a different random domain must not alias it.
  for (std::size_t offset = 0; offset < forward.size(); ++offset) {
    const std::size_t index = forward.size() - 1 - offset;
    const uint64_t simulation_id = context.simulation_id_base + index;
    if (context.seed_for(SearchRandomDomain::RootDeterminization,
                         simulation_id) != forward[index])
      return false;
    if (context.seed_for(SearchRandomDomain::RootDirichlet, simulation_id) ==
        forward[index])
      return false;
  }
  return true;
}

bool test_search_guard_and_config_lifecycle() {
  MCTSConfig initial;
  initial.cpuct = 1.25f;
  MCTS mcts(initial);

  MCTSConfig detached = mcts.get_config_snapshot();
  detached.cpuct = 9.0f;
  if (mcts.get_config_snapshot().cpuct != 1.25f)
    return false;
  mcts.set_config(detached);
  if (mcts.get_config_snapshot().cpuct != 9.0f)
    return false;

  const uint64_t initial_generation = mcts.tree_generation_snapshot();
  auto first = mcts.begin_parallel_search();
  if (!first.active() || !mcts.is_parallel_search_active() ||
      first.tree_generation() != initial_generation ||
      first.search_nonce() != 0 || first.config().cpuct != 9.0f)
    return false;
  if (!throws_logic_error([&] { (void)mcts.begin_parallel_search(); }))
    return false;

  MCTSConfig replacement = detached;
  replacement.cpuct = 3.0f;
  if (!throws_logic_error([&] { mcts.clear(); }) ||
      !throws_logic_error([&] { mcts.set_config(replacement); }) ||
      !throws_logic_error([&] { mcts.prune_if_needed(); }) ||
      !throws_logic_error([&] { (void)mcts.get_or_create_node(123); }) ||
      !throws_logic_error([&] { mcts.update_stats(123, 0, 1.0f); }))
    return false;

  MCTS::SearchGuard moved(std::move(first));
  if (first.active() || !moved.active() || !mcts.is_parallel_search_active())
    return false;
  moved.finish();
  if (moved.active() || mcts.is_parallel_search_active() ||
      !throws_logic_error([&] { moved.finish(); }))
    return false;

  // Idle operations are reusable after the guard releases the session.
  mcts.set_config(replacement);
  mcts.clear();
  const uint64_t after_clear_generation = mcts.tree_generation_snapshot();
  if (after_clear_generation != initial_generation + 1)
    return false;
  {
    auto second = mcts.begin_parallel_search();
    if (second.search_nonce() != 1 ||
        second.tree_generation() != after_clear_generation)
      return false;
    // Destructor fallback releases Active -> Idle.
  }
  if (mcts.is_parallel_search_active())
    return false;

  // A tree-domain change invalidates both physically separate stores.
  replacement.use_determinization = !replacement.use_determinization;
  mcts.set_config(replacement);
  return mcts.tree_generation_snapshot() == after_clear_generation + 1;
}

bool test_replay_sequence_lifecycle() {
  MCTSConfig config;
  MCTS mcts(config);

  uint64_t session_seed = 0;
  {
    auto first = mcts.begin_parallel_search();
    session_seed = first.resolved_master_seed();
    if (first.search_nonce() != 0 ||
        !throws_logic_error([&] { mcts.reset_replay_sequence(123, 456); }))
      return false;

    auto &tree = mcts.prepare_parallel_tree(
        first, mcts_parallel::TreeBackend::Coarse, 1);
    (void)tree.find_or_create(exact_key(0x1234));
    first.finish();
  }

  const uint64_t retained_generation = mcts.tree_generation_snapshot();
  if (mcts.parallel_tree_size() != 1)
    return false;
  try {
    mcts.reset_replay_sequence(999, std::numeric_limits<uint64_t>::max());
    return false;
  } catch (const std::overflow_error &) {
  }
  {
    auto second = mcts.begin_parallel_search();
    if (second.resolved_master_seed() != session_seed ||
        second.search_nonce() != 1)
      return false;
    second.finish();
  }

  // Seed zero is a real replay seed. Resetting the replay identity must not
  // clear either the retained parallel tree or its generation.
  mcts.reset_replay_sequence(0, 17);
  if (mcts.tree_generation_snapshot() != retained_generation ||
      mcts.parallel_tree_size() != 1)
    return false;
  {
    auto replay = mcts.begin_parallel_search();
    if (replay.resolved_master_seed() != 0 || replay.search_nonce() != 17 ||
        replay.tree_generation() != retained_generation)
      return false;
    replay.finish();
  }

  // Ordinary tree lifecycle operations never reset the replay sequence.
  mcts.clear();
  if (mcts.tree_generation_snapshot() != retained_generation + 1 ||
      mcts.parallel_tree_size() != 0)
    return false;
  auto after_clear = mcts.begin_parallel_search();
  return after_clear.resolved_master_seed() == 0 &&
         after_clear.search_nonce() == 18;
}

bool test_reservation_commit_abort_and_destructor() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 7;
  ConcurrentTree tree(generation);
  const NodeHandle node = tree.find_or_create(exact_key(100));
  const ActionMask mask = mask_with({4});
  tree.expand(node, policy_with({{4, 1.0f}}), test_value(), mask);

  SelectionContext context;
  context.tree_generation = generation;
  auto ledger = std::make_shared<SearchLedger>();

  auto selected =
      tree.select_and_reserve(node, mask, context, NUM_PLAYERS - 1, ledger);
  if (!selected || selected->action() != 4 || !selected->live())
    return false;
  SelectionReservation moved(std::move(*selected));
  if (selected->live() || !moved.live())
    return false;
  moved.commit(0.75, generation);
  auto snapshot = tree.snapshot(node);
  if (snapshot.stats.virtual_loss[4] != 0 || snapshot.stats.N[4] != 1 ||
      snapshot.stats.total_visits != 1 ||
      std::abs(snapshot.stats.Q[4] - 0.75) > 1e-12 ||
      !throws_logic_error([&] { moved.commit(0.25, generation); }) ||
      !throws_logic_error([&] { moved.abort(); }))
    return false;

  auto aborted = tree.select_and_reserve(node, mask, context, 0, ledger);
  if (!aborted)
    return false;
  aborted->abort();
  snapshot = tree.snapshot(node);
  if (snapshot.stats.virtual_loss[4] != 0 || snapshot.stats.N[4] != 1 ||
      !throws_logic_error([&] { aborted->abort(); }) ||
      !throws_logic_error([&] { aborted->commit(0.0, generation); }))
    return false;

  {
    auto abandoned = tree.select_and_reserve(node, mask, context, 0, ledger);
    if (!abandoned || tree.snapshot(node).stats.virtual_loss[4] != 1)
      return false;
  }
  snapshot = tree.snapshot(node);
  const SearchLedgerSnapshot counters = ledger->snapshot();
  tree.validate_quiescent();
  return snapshot.stats.virtual_loss[4] == 0 && snapshot.stats.N[4] == 1 &&
         counters.virtual_loss_added == 3 &&
         counters.virtual_loss_released == 3 &&
         counters.reservations_committed == 1 &&
         counters.reservations_aborted == 2 && counters.virtual_loss_balanced();
}

bool test_multiple_tokens_and_reserved_path_validation() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 11;
  ConcurrentTree tree(generation);
  const NodeHandle node = tree.find_or_create(exact_key(200));
  const ActionMask only_edge = mask_with({8});
  tree.expand(node, policy_with({{8, 1.0f}}), test_value(), only_edge);
  SelectionContext context;
  context.tree_generation = generation;
  auto ledger = std::make_shared<SearchLedger>();

  auto first = tree.select_and_reserve(node, only_edge, context, 0, ledger);
  auto second = tree.select_and_reserve(node, only_edge, context, 1, ledger);
  if (!first || !second ||
      first->reservation_id() == second->reservation_id() ||
      tree.snapshot(node).stats.virtual_loss[8] != 2)
    return false;
  first->commit(0.5, generation);
  second->abort();
  auto snapshot = tree.snapshot(node);
  if (snapshot.stats.N[8] != 1 || snapshot.stats.total_visits != 1 ||
      snapshot.stats.virtual_loss[8] != 0)
    return false;

  // Generation equality is not ownership. A foreign stable handle must be
  // rejected before this tree takes an unrelated node lock.
  ConcurrentTree same_generation(generation);
  const NodeHandle foreign = same_generation.find_or_create(exact_key(299));
  if (!throws_logic_error([&] { (void)tree.snapshot(foreign); }) ||
      !throws_logic_error([&] {
        tree.expand(foreign, policy_with({{8, 1.0f}}), test_value(), only_edge);
      }))
    return false;

  // Mix generations deliberately. commit() must validate the complete path
  // before changing the first valid entry.
  ConcurrentTree other(generation + 1);
  const NodeHandle other_node = other.find_or_create(exact_key(201));
  other.expand(other_node, policy_with({{8, 1.0f}}), test_value(), only_edge);
  SelectionContext other_context = context;
  other_context.tree_generation = generation + 1;
  auto valid = tree.select_and_reserve(node, only_edge, context, 0, ledger);
  auto stale_for_expected =
      other.select_and_reserve(other_node, only_edge, other_context, 0, ledger);
  if (!valid || !stale_for_expected)
    return false;
  {
    ReservedPath path;
    path.append(std::move(*valid));
    path.append(std::move(*stale_for_expected));
    const Value value = test_value(0.9);
    if (!throws_logic_error([&] { path.commit(value, generation); }))
      return false;
    if (tree.snapshot(node).stats.N[8] != 1 ||
        tree.snapshot(node).stats.virtual_loss[8] != 1 ||
        other.snapshot(other_node).stats.N[8] != 0 ||
        other.snapshot(other_node).stats.virtual_loss[8] != 1)
      return false;
    // Destructor releases both still-live entries.
  }
  if (tree.snapshot(node).stats.virtual_loss[8] != 0 ||
      other.snapshot(other_node).stats.virtual_loss[8] != 0)
    return false;

  // A generation invalidation is stale even while the strong NodeHandle keeps
  // the old record alive; statistics remain unchanged.
  auto invalidated =
      tree.select_and_reserve(node, only_edge, context, 0, ledger);
  if (!invalidated)
    return false;
  tree.clear_and_set_generation(generation + 2);
  if (!throws_logic_error([&] { invalidated->commit(1.0, generation + 2); }))
    return false;
  if (tree.snapshot(node).stats.N[8] != 1 ||
      tree.snapshot(node).stats.virtual_loss[8] != 1)
    return false;
  invalidated.reset();
  return tree.snapshot(node).stats.virtual_loss[8] == 0;
}

bool test_world_mask_and_unmasked_base_policy() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 19;
  ConcurrentTree tree(generation);
  const NodeHandle node = tree.find_or_create(exact_key(300));
  const Policy base = policy_with({{2, 0.01f}, {17, 0.99f}});
  tree.expand(node, base, test_value(), mask_with({2}));
  SelectionContext context;
  context.tree_generation = generation;

  auto first =
      tree.select_and_reserve(node, mask_with({2}), context, 0, nullptr);
  if (!first || first->action() != 2)
    return false;
  first->abort();
  auto later =
      tree.select_and_reserve(node, mask_with({17}), context, 0, nullptr);
  if (!later || later->action() != 17)
    return false;
  later->abort();

  const auto snapshot = tree.snapshot(node);
  return snapshot.valid_actions[2] == 1 && snapshot.valid_actions[17] == 1 &&
         snapshot.base_policy[2] == base[2] &&
         snapshot.base_policy[17] == base[17] &&
         snapshot.availability_count[2] == 1 &&
         snapshot.availability_count[17] == 1;
}

bool test_pending_owner_waiter_publish_and_cancel() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 23;
  ConcurrentTree tree(generation);
  auto ledger = std::make_shared<SearchLedger>();
  const NodeHandle node = tree.find_or_create(exact_key(400));
  std::array<float, FEATURE_SIZE> features{};
  features[7] = 1.0f;
  const ActionMask owner_mask = mask_with({1});
  const ActionMask waiter_mask = mask_with({9});

  ExpansionClaim owner =
      tree.claim_or_attach(node, 100, 0xabc, features, owner_mask, ledger);
  if (owner.kind != ExpansionClaimKind::Owner || !owner.pending ||
      owner.pending->request.owner_world_mask != owner_mask)
    return false;
  ExpansionClaim waiter =
      tree.claim_or_attach(node, 101, 0xabc, features, waiter_mask, ledger);
  if (waiter.kind != ExpansionClaimKind::Waiter ||
      waiter.pending.get() != owner.pending.get())
    return false;

  if (!throws_logic_error([&] {
        (void)tree.claim_or_attach(node, 102, 0xdef, features, waiter_mask,
                                   ledger);
      }))
    return false;

  Policy policy = policy_with({{1, 0.2f}, {9, 0.8f}});
  const Value value = test_value(0.4);
  const std::vector<uint64_t> attached =
      tree.publish_evaluation(node, owner.pending, policy, value, ledger);
  if (attached != std::vector<uint64_t>({100, 101}) ||
      owner.pending->state != PendingState::Published)
    return false;
  const auto published = tree.snapshot(node);
  if (published.state != ExpansionState::Expanded ||
      published.base_policy != policy || published.stats.value != value ||
      !throws_logic_error([&] {
        (void)tree.publish_evaluation(node, owner.pending, policy, value,
                                      ledger);
      }))
    return false;

  // A late arrival observes the completed node instead of attaching to a
  // closing/published PendingEvaluation.
  ExpansionClaim late =
      tree.claim_or_attach(node, 103, 0xabc, features, waiter_mask, ledger);
  if (late.kind != ExpansionClaimKind::Expanded || late.pending ||
      late.cached_value != value)
    return false;

  const NodeHandle cancelled_node = tree.find_or_create(exact_key(401));
  ExpansionClaim cancelled = tree.claim_or_attach(cancelled_node, 200, 0x123,
                                                  features, owner_mask, ledger);
  if (cancelled.kind != ExpansionClaimKind::Owner || !cancelled.pending)
    return false;
  const std::vector<uint64_t> cancelled_tickets = tree.fail_evaluation(
      cancelled_node, cancelled.pending, PendingState::Cancelled);
  if (cancelled_tickets != std::vector<uint64_t>({200}) ||
      cancelled.pending->state != PendingState::Cancelled ||
      tree.snapshot(cancelled_node).state != ExpansionState::Unexpanded)
    return false;
  if (!throws_logic_error([&] {
        (void)tree.fail_evaluation(cancelled_node, cancelled.pending,
                                   PendingState::Cancelled);
      }))
    return false;

  // Cancellation leaves the node claimable, never permanently Evaluating.
  ExpansionClaim retry = tree.claim_or_attach(cancelled_node, 201, 0x123,
                                              features, waiter_mask, ledger);
  if (retry.kind != ExpansionClaimKind::Owner || !retry.pending)
    return false;
  (void)tree.fail_evaluation(cancelled_node, retry.pending,
                             PendingState::Cancelled);
  const SearchLedgerSnapshot counters = ledger->snapshot();
  return counters.evaluation_owner == 3 && counters.evaluation_waiter == 1 &&
         counters.expansion_claimed == 3 && counters.expansion_published == 1 &&
         counters.evaluated_boards == 1;
}

bool test_64_bit_stats_and_legacy_overflow() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 29;
  ConcurrentTree tree(generation);
  const NodeHandle node = tree.find_or_create(exact_key(500));
  const ActionMask mask = mask_with({6});
  tree.expand(node, policy_with({{6, 1.0f}}), test_value(), mask);
  const uint64_t legacy_max =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
  tree.set_stats_for_testing(node, 6, legacy_max, 0.5);
  const MCTSNode boundary = to_legacy_node(tree.snapshot(node));
  if (boundary.N[6] != std::numeric_limits<uint32_t>::max() ||
      boundary.total_visits != std::numeric_limits<uint32_t>::max())
    return false;

  SelectionContext context;
  context.tree_generation = generation;
  auto reservation = tree.select_and_reserve(node, mask, context, 0, nullptr);
  if (!reservation)
    return false;
  reservation->commit(0.5, generation);
  const auto beyond = tree.snapshot(node);
  if (beyond.stats.N[6] != legacy_max + 1 ||
      beyond.stats.total_visits != legacy_max + 1)
    return false;
  try {
    (void)to_legacy_node(beyond);
    return false;
  } catch (const std::overflow_error &) {
  }

  uint64_t sum = 0;
  for (uint64_t visits : beyond.stats.N)
    sum += visits;
  tree.validate_quiescent();
  return sum == beyond.stats.total_visits;
}

bool test_reservation_counter_overflow_is_atomic() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 30;
  constexpr uint64_t counter_max = std::numeric_limits<uint64_t>::max();

  const auto same_snapshot = [](const MCTSNodeSnapshot64 &left,
                                const MCTSNodeSnapshot64 &right) {
    return left.key == right.key && left.generation == right.generation &&
           left.valid_actions == right.valid_actions &&
           left.base_policy == right.base_policy &&
           left.stats.N == right.stats.N &&
           left.stats.virtual_loss == right.stats.virtual_loss &&
           left.stats.Q == right.stats.Q &&
           left.stats.total_visits == right.stats.total_visits &&
           left.stats.value == right.stats.value &&
           left.availability_count == right.availability_count &&
           left.live_reservation_count == right.live_reservation_count &&
           left.has_pending_evaluation == right.has_pending_evaluation &&
           left.state == right.state;
  };
  const auto selection_counters_unchanged =
      [](const SearchLedgerSnapshot &left, const SearchLedgerSnapshot &right) {
        return left.selected == right.selected &&
               left.virtual_loss_added == right.virtual_loss_added &&
               left.virtual_loss_released == right.virtual_loss_released &&
               left.reservations_committed == right.reservations_committed &&
               left.reservations_aborted == right.reservations_aborted &&
               left.integrity_errors == right.integrity_errors;
      };
  const auto rejects_without_mutation =
      [&](ConcurrentTree &tree, const NodeHandle &node,
          const ActionMask &world_mask, const SelectionContext &context,
          const std::shared_ptr<SearchLedger> &ledger) {
        const auto before = tree.snapshot(node);
        const auto ledger_before = ledger->snapshot();
        try {
          (void)tree.select_and_reserve(node, world_mask, context, 0, ledger);
          return false;
        } catch (const std::overflow_error &) {
        }
        return same_snapshot(before, tree.snapshot(node)) &&
               selection_counters_unchanged(ledger_before, ledger->snapshot());
      };

  SelectionContext context;
  context.tree_generation = generation;
  auto ledger = std::make_shared<SearchLedger>();

  // The overflowing candidate comes after another candidate. Neither its
  // predecessor's union/count nor any reservation state may be updated.
  ConcurrentTree availability_tree(generation);
  const NodeHandle availability_node =
      availability_tree.find_or_create(exact_key(510));
  availability_tree.expand(availability_node,
                           policy_with({{8, 0.5f}, {9, 0.5f}}), test_value(),
                           mask_with({9}));
  availability_tree.set_selection_counters_for_testing(availability_node, 9, 0,
                                                       0, 0, counter_max);
  if (!rejects_without_mutation(availability_tree, availability_node,
                                mask_with({8, 9}), context, ledger))
    return false;

  ConcurrentTree action_visits_tree(generation);
  const NodeHandle action_visits_node =
      action_visits_tree.find_or_create(exact_key(511));
  action_visits_tree.expand(action_visits_node, policy_with({{10, 1.0f}}),
                            test_value(), mask_with({10}));
  action_visits_tree.set_selection_counters_for_testing(action_visits_node, 10,
                                                        counter_max, 0, 0, 0);
  if (!rejects_without_mutation(action_visits_tree, action_visits_node,
                                mask_with({10}), context, ledger))
    return false;

  ConcurrentTree future_action_visits_tree(generation);
  const NodeHandle future_action_visits_node =
      future_action_visits_tree.find_or_create(exact_key(515));
  future_action_visits_tree.expand(future_action_visits_node,
                                   policy_with({{10, 1.0f}}), test_value(),
                                   mask_with({10}));
  future_action_visits_tree.set_selection_counters_for_testing(
      future_action_visits_node, 10, counter_max - 1, 0, 1, 0);
  if (!rejects_without_mutation(future_action_visits_tree,
                                future_action_visits_node, mask_with({10}),
                                context, ledger))
    return false;

  ConcurrentTree virtual_loss_tree(generation);
  const NodeHandle virtual_loss_node =
      virtual_loss_tree.find_or_create(exact_key(512));
  virtual_loss_tree.expand(virtual_loss_node, policy_with({{11, 1.0f}}),
                           test_value(), mask_with({11}));
  virtual_loss_tree.set_selection_counters_for_testing(virtual_loss_node, 11, 0,
                                                       0, counter_max, 0);
  if (!rejects_without_mutation(virtual_loss_tree, virtual_loss_node,
                                mask_with({11}), context, ledger))
    return false;

  ConcurrentTree node_visits_tree(generation);
  const NodeHandle node_visits_node =
      node_visits_tree.find_or_create(exact_key(513));
  node_visits_tree.expand(node_visits_node, policy_with({{12, 1.0f}}),
                          test_value(), mask_with({12}));
  node_visits_tree.set_selection_counters_for_testing(node_visits_node, 12, 0,
                                                      counter_max, 0, 0);
  if (!rejects_without_mutation(node_visits_tree, node_visits_node,
                                mask_with({12}), context, ledger))
    return false;

  // At max-1 exactly one live reservation is safe. A second reservation must
  // be rejected without incrementing availability or virtual loss; committing
  // the first reaches max without wrapping either visit counter.
  ConcurrentTree boundary_tree(generation);
  const NodeHandle boundary_node = boundary_tree.find_or_create(exact_key(514));
  boundary_tree.expand(boundary_node, policy_with({{13, 1.0f}}), test_value(),
                       mask_with({13}));
  boundary_tree.set_selection_counters_for_testing(
      boundary_node, 13, counter_max - 1, counter_max - 1, 0, 0);
  auto final_slot = boundary_tree.select_and_reserve(
      boundary_node, mask_with({13}), context, 0, ledger);
  if (!final_slot ||
      !rejects_without_mutation(boundary_tree, boundary_node, mask_with({13}),
                                context, ledger))
    return false;
  final_slot->commit(0.5, generation);
  const auto at_max = boundary_tree.snapshot(boundary_node);
  if (at_max.stats.N[13] != counter_max ||
      at_max.stats.total_visits != counter_max ||
      at_max.stats.virtual_loss[13] != 0 ||
      at_max.live_reservation_count != 0 ||
      !rejects_without_mutation(boundary_tree, boundary_node, mask_with({13}),
                                context, ledger))
    return false;

  const SearchLedgerSnapshot counters = ledger->snapshot();
  return counters.selected == 1 && counters.virtual_loss_added == 1 &&
         counters.virtual_loss_released == 1 &&
         counters.reservations_committed == 1 &&
         counters.reservations_aborted == 0 && counters.integrity_errors == 0;
}

bool test_expansion_and_terminal_state_transitions() {
  using namespace mcts_parallel;
  constexpr uint64_t generation = 31;
  ConcurrentTree tree(generation);
  const Policy policy = policy_with({{3, 1.0f}});
  const ActionMask mask = mask_with({3});
  const Value value = test_value(0.2);

  const NodeHandle expanded = tree.find_or_create(exact_key(600));
  tree.expand(expanded, policy, value, mask);
  if (!throws_logic_error([&] { tree.expand(expanded, policy, value, mask); }))
    return false;
  const auto expanded_snapshot = tree.snapshot(expanded);
  if (expanded_snapshot.state != ExpansionState::Expanded ||
      expanded_snapshot.base_policy != policy ||
      expanded_snapshot.stats.value != value ||
      !throws_logic_error([&] { tree.set_terminal(expanded, value); }))
    return false;

  const NodeHandle terminal = tree.find_or_create(exact_key(601));
  tree.set_terminal(terminal, value);
  // Re-observing the same terminal result is deliberately idempotent.
  tree.set_terminal(terminal, value);
  Value inconsistent = value;
  inconsistent[0] += 0.5;
  if (!throws_logic_error([&] { tree.set_terminal(terminal, inconsistent); }) ||
      !throws_logic_error([&] { tree.expand(terminal, policy, value, mask); }))
    return false;
  const auto terminal_snapshot = tree.snapshot(terminal);
  tree.validate_quiescent();
  return terminal_snapshot.state == ExpansionState::Terminal &&
         terminal_snapshot.stats.value == value &&
         terminal_snapshot.stats.total_visits == 0;
}

} // namespace

int main() {
  if (!test_tree_key_domains())
    return 10;
  if (!test_hidden_tier_hash())
    return 11;
  if (!test_hidden_multi_world_rejected_before_mutation())
    return 12;
  if (!test_portable_rng_golden())
    return 13;
  if (!test_portable_determinization_contract())
    return 14;
  if (!test_seed_manifest_is_logical_id_based())
    return 15;
  if (!test_search_guard_and_config_lifecycle())
    return 16;
  if (!test_reservation_commit_abort_and_destructor())
    return 17;
  if (!test_multiple_tokens_and_reserved_path_validation())
    return 18;
  if (!test_world_mask_and_unmasked_base_policy())
    return 19;
  if (!test_pending_owner_waiter_publish_and_cancel())
    return 20;
  if (!test_64_bit_stats_and_legacy_overflow())
    return 21;
  if (!test_reservation_counter_overflow_is_atomic())
    return 24;
  if (!test_expansion_and_terminal_state_transitions())
    return 22;
  if (!test_replay_sequence_lifecycle())
    return 23;
  return 0;
}
