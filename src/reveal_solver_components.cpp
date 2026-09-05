#include "reveal_solver_components.h"

#include "card_data.h"
#include "perf_counters.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace csplendor::solver_internal {

void CardIdSet::clear() noexcept {
  low = 0;
  high = 0;
}

void CardIdSet::add(int card_id) noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return;
  if (card_id < 64)
    low |= uint64_t{1} << card_id;
  else
    high |= uint64_t{1} << (card_id - 64);
}

void CardIdSet::remove(int card_id) noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return;
  if (card_id < 64)
    low &= ~(uint64_t{1} << card_id);
  else
    high &= ~(uint64_t{1} << (card_id - 64));
}

bool CardIdSet::contains(int card_id) const noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return false;
  if (card_id < 64)
    return (low & (uint64_t{1} << card_id)) != 0;
  return (high & (uint64_t{1} << (card_id - 64))) != 0;
}

bool CardIdSet::operator==(const CardIdSet &other) const noexcept {
  return low == other.low && high == other.high;
}

bool CardIdSet::operator!=(const CardIdSet &other) const noexcept {
  return !(*this == other);
}

void HiddenOutcomeCatalog::remember_initial_position(
    const Game &game) noexcept {
  known_.clear();
  hidden_.clear();
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      known_.add(game.board.visible[level][slot]);
    for (uint8_t card_id : game.board.decks[level])
      hidden_.add(card_id);
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      known_.add(game.board.players[player].reserved[slot]);
  }
}

bool HiddenOutcomeCatalog::is_initially_known(int card_id) const noexcept {
  return known_.contains(card_id);
}

bool HiddenOutcomeCatalog::is_initially_hidden(int card_id) const noexcept {
  return hidden_.contains(card_id);
}

bool HiddenOutcomeCatalog::is_claimed(const Board &board,
                                      int card_id) noexcept {
  CSPLENDOR_PERF_INC(SolverIsClaimedCalls);
  if (card_id < 0 || card_id >= CARD_COUNT)
    return false;
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    const auto &cards = board.players[player].purchased_cards;
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    for (uint8_t purchased : cards) {
      CSPLENDOR_PERF_INC(SolverIsClaimedComparisons);
      if (purchased == static_cast<uint8_t>(card_id))
        return true;
    }
#else
    if (std::find(cards.begin(), cards.end(), static_cast<uint8_t>(card_id)) !=
        cards.end())
      return true;
#endif
  }
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      CSPLENDOR_PERF_INC(SolverIsClaimedComparisons);
      if (board.visible[level][slot] == card_id)
        return true;
    }
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
      CSPLENDOR_PERF_INC(SolverIsClaimedComparisons);
      if (board.players[player].reserved[slot] == card_id)
        return true;
    }
  }
  return false;
}

CardIdSet HiddenOutcomeCatalog::claimed_cards(const Board &board) noexcept {
  CardIdSet result;
  for (const PlayerState &player : board.players) {
    for (uint8_t card_id : player.purchased_cards)
      result.add(card_id);
    for (int card_id : player.reserved)
      result.add(card_id);
  }
  for (const auto &level : board.visible) {
    for (int card_id : level)
      result.add(card_id);
  }
  return result;
}

CardIdSet
HiddenOutcomeCatalog::unseen_cards(const Board &board) const noexcept {
  CardIdSet result;
  for (int level = 0; level < 3; ++level) {
    for (uint8_t card_id : board.decks[level]) {
      CSPLENDOR_PERF_INC(SolverScannedDeckCards);
      result.add(card_id);
    }
  }
  return result;
}

CardIdSet
HiddenOutcomeCatalog::acquired_hidden_cards(const Board &board) const noexcept {
  CardIdSet result;
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (uint8_t card_id : board.players[player].purchased_cards) {
      CSPLENDOR_PERF_INC(SolverScannedPurchasedIds);
      if (is_initially_hidden(card_id))
        result.add(card_id);
    }
  }
  return result;
}

namespace {

uint64_t packed_colours(const std::array<uint8_t, 5> &values) noexcept {
  uint64_t packed = 0;
  for (size_t color = 0; color < values.size(); ++color)
    packed |= static_cast<uint64_t>(values[color]) << (color * 12U);
  return packed;
}

uint64_t packed_player_gems(const PlayerState &player) noexcept {
  uint64_t packed = 0;
  for (size_t color = 0; color < 5; ++color)
    packed |= static_cast<uint64_t>(player.gems[color]) << (color * 12U);
  return packed;
}

bool remember_unique_card(std::array<uint8_t, CARD_COUNT> &seen,
                          int card_id) noexcept {
  if (!is_valid_card_id(card_id))
    return false;
  return seen[static_cast<size_t>(card_id)]++ == 0;
}

bool remember_unique_noble(std::array<uint8_t, NOBLE_COUNT> &seen,
                           int noble_id) noexcept {
  if (!is_valid_noble_id(noble_id))
    return false;
  return seen[static_cast<size_t>(noble_id)]++ == 0;
}

} // namespace

bool RevealSearchState::canonical_root(const Board &board) noexcept {
  if (board.current_player >= Board::NUM_PLAYERS || board.winner < -2 ||
      board.winner > 1)
    return false;

  std::array<uint8_t, CARD_COUNT> seen_cards{};
  size_t card_count = 0;
  for (int level = 0; level < 3; ++level) {
    if (board.decks[level].count > Board::MAX_DECK_SIZE)
      return false;
    for (int card_id : board.visible[level]) {
      if (card_id == -1)
        continue;
      if (!remember_unique_card(seen_cards, card_id) ||
          get_card(card_id).level != level + 1)
        return false;
      ++card_count;
    }
    for (uint8_t card_id : board.decks[level]) {
      if (!remember_unique_card(seen_cards, card_id) ||
          get_card(card_id).level != level + 1)
        return false;
      ++card_count;
    }
  }

  std::array<uint8_t, NOBLE_COUNT> seen_nobles{};
  if (board.nobles.count > Board::MAX_NOBLES_ON_BOARD)
    return false;
  size_t noble_count = 0;
  for (uint8_t noble_id : board.nobles) {
    if (!remember_unique_noble(seen_nobles, noble_id))
      return false;
    ++noble_count;
  }

  std::array<int, 6> token_totals{};
  for (size_t color = 0; color < token_totals.size(); ++color)
    token_totals[color] = board.bank[color];

  for (const PlayerState &player : board.players) {
    if (player.reserved_count > Board::MAX_RESERVED ||
        player.purchased_cards.size() > CARD_COUNT ||
        player.acquired_nobles.size() > NOBLE_COUNT ||
        player.purchased_count != player.purchased_cards.size() ||
        player.packed_gems != packed_player_gems(player) ||
        player.packed_bonuses != packed_colours(player.bonuses) ||
        player.noble_eligibility_mask !=
            noble_eligibility_mask_from_bonuses(player.bonuses) ||
        player.total_gems() > Board::MAX_TOKENS)
      return false;

    size_t occupied_reserved = 0;
    bool found_empty = false;
    for (size_t slot = 0; slot < player.reserved.size(); ++slot) {
      const int card_id = player.reserved[slot];
      if (card_id == -1) {
        found_empty = true;
        if (player.reserved_is_hidden[slot])
          return false;
        continue;
      }
      if (found_empty || !remember_unique_card(seen_cards, card_id))
        return false;
      ++occupied_reserved;
      ++card_count;
    }
    if (occupied_reserved != player.reserved_count)
      return false;

    std::array<int, 5> expected_bonuses{};
    int expected_points = 0;
    for (uint8_t card_id : player.purchased_cards) {
      if (!remember_unique_card(seen_cards, card_id))
        return false;
      const Card &card = get_card(card_id);
      ++expected_bonuses[card.bonus];
      expected_points += card.points;
      ++card_count;
    }
    for (size_t color = 0; color < expected_bonuses.size(); ++color) {
      if (player.bonuses[color] != expected_bonuses[color])
        return false;
    }
    for (uint8_t noble_id : player.acquired_nobles) {
      if (!remember_unique_noble(seen_nobles, noble_id))
        return false;
      expected_points += get_noble(noble_id).points;
      ++noble_count;
    }
    if (player.points != expected_points)
      return false;
    for (size_t color = 0; color < token_totals.size(); ++color)
      token_totals[color] += player.gems[color];
  }

  constexpr std::array<int, 6> EXPECTED_TOKENS = {
      Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR,
      Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR, Board::NUM_GOLD};
  return card_count == CARD_COUNT && noble_count == Board::NUM_NOBLES &&
         token_totals == EXPECTED_TOKENS;
}

uint64_t RevealSearchState::deck_order_hash(const Board &board) noexcept {
  const Zobrist &zobrist = Zobrist::get_instance();
  uint64_t hash = 0;
  for (int level = 0; level < 3; ++level) {
    for (size_t position = 0; position < board.decks[level].size();
         ++position) {
      hash ^= Board::exact_deck_card_salt(
          zobrist, level, position, board.decks[level][position]);
    }
  }
  return hash;
}

bool RevealSearchState::initialize(
    const Game &game, const HiddenOutcomeCatalog &catalog) noexcept {
  deactivate();
  if (!config::incremental_reveal_search_state_enabled ||
      !canonical_root(game.board)) {
    CSPLENDOR_PERF_INC(SolverRevealStateFallbackInitializations);
    return false;
  }

  for (int level = 0; level < 3; ++level) {
    for (uint8_t card_id : game.board.decks[level]) {
      remaining_by_level_[level].add(card_id);
      remaining_all_.add(card_id);
    }
  }
  acquired_hidden_ = catalog.acquired_hidden_cards(game.board);
  claimed_ = HiddenOutcomeCatalog::claimed_cards(game.board);
  rule_hash_ = game.board.compute_set_deck_search_hash();
  deck_order_hash_ = deck_order_hash(game.board);
  const uint64_t exact_hash = game.board.hash();
  const uint64_t turn_hash = Board::exact_turn_salt(
      Zobrist::get_instance(), game.board.turn);
  if ((exact_hash ^ deck_order_hash_ ^ turn_hash) != rule_hash_) {
    deactivate();
    CSPLENDOR_PERF_INC(SolverRevealStateFallbackInitializations);
    return false;
  }
  active_ = true;
  CSPLENDOR_PERF_INC(SolverRevealStateFastInitializations);
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
  verify_or_abort(game.board, catalog);
#endif
  return true;
}

bool RevealSearchState::active() const noexcept { return active_; }

const std::array<CardIdSet, 3> &
RevealSearchState::remaining_by_level() const noexcept {
  return remaining_by_level_;
}

const CardIdSet &RevealSearchState::remaining_all() const noexcept {
  return remaining_all_;
}

const CardIdSet &RevealSearchState::acquired_hidden() const noexcept {
  return acquired_hidden_;
}

const CardIdSet &RevealSearchState::claimed() const noexcept {
  return claimed_;
}

uint64_t RevealSearchState::rule_hash() const noexcept { return rule_hash_; }

bool RevealSearchState::is_claimed(const Board &board,
                                   int card_id) const noexcept {
  return active_ ? claimed_.contains(card_id)
                 : HiddenOutcomeCatalog::is_claimed(board, card_id);
}

RevealSearchState::TransitionObservation
RevealSearchState::observe_before(const Board &board) const noexcept {
  TransitionObservation observation;
  for (int level = 0; level < 3; ++level) {
    observation.deck_sizes[level] = board.decks[level].size();
    if (!board.decks[level].empty())
      observation.deck_tops[level] = board.decks[level].back();
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player)
    observation.purchased_sizes[player] =
        board.players[player].purchased_cards.size();
  return observation;
}

void RevealSearchState::observe_after(
    const TransitionObservation &before, const Board &board,
    const HiddenOutcomeCatalog &catalog) noexcept {
  if (!active_)
    return;

  for (int level = 0; level < 3; ++level) {
    const size_t after_size = board.decks[level].size();
    if (after_size == before.deck_sizes[level])
      continue;
    if (before.deck_sizes[level] == 0 ||
        after_size + 1 != before.deck_sizes[level] ||
        !is_valid_card_id(before.deck_tops[level])) {
      runtime_fallback("unexpected deck-size transition");
      return;
    }
    const int card_id = before.deck_tops[level];
    remaining_by_level_[level].remove(card_id);
    remaining_all_.remove(card_id);
    deck_order_hash_ ^= Board::exact_deck_card_salt(
        Zobrist::get_instance(), level, after_size,
        static_cast<uint8_t>(card_id));
  }

  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    const auto &purchased = board.players[player].purchased_cards;
    if (purchased.size() < before.purchased_sizes[player]) {
      runtime_fallback("purchased-card vector shrank");
      return;
    }
    for (size_t index = before.purchased_sizes[player];
         index < purchased.size(); ++index) {
      const int card_id = purchased[index];
      claimed_.add(card_id);
      if (catalog.is_initially_hidden(card_id))
        acquired_hidden_.add(card_id);
    }
    for (int card_id : board.players[player].reserved)
      claimed_.add(card_id);
  }
  for (const auto &level : board.visible) {
    for (int card_id : level)
      claimed_.add(card_id);
  }

  if (!board.hash_valid) {
    runtime_fallback("exact board hash is unavailable");
    return;
  }
  rule_hash_ = board.cached_hash ^ deck_order_hash_ ^
               Board::exact_turn_salt(Zobrist::get_instance(), board.turn);
  CSPLENDOR_PERF_INC(SolverRevealStateTransitions);
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
  verify_or_abort(board, catalog);
#endif
}

bool RevealSearchState::move_deck_card_to_back(Board &board, int level,
                                                int card_id) noexcept {
  if (!active_ || level < 0 || level >= 3 || !is_valid_card_id(card_id))
    return false;
  if (!board.hash_valid) {
    runtime_fallback("exact board hash is unavailable before reveal");
    return false;
  }
  auto &deck = board.decks[level];
  const auto found =
      std::find(deck.begin(), deck.end(), static_cast<uint8_t>(card_id));
  if (found == deck.end())
    return false;
  const size_t index = static_cast<size_t>(found - deck.begin());
  if (index + 1 == deck.size())
    return true;

  const Zobrist &zobrist = Zobrist::get_instance();
  uint64_t delta = 0;
  for (size_t position = index; position < deck.size(); ++position) {
    delta ^= Board::exact_deck_card_salt(zobrist, level, position,
                                         deck[position]);
  }
  std::rotate(deck.begin() + static_cast<std::ptrdiff_t>(index),
              deck.begin() + static_cast<std::ptrdiff_t>(index + 1),
              deck.end());
  for (size_t position = index; position < deck.size(); ++position) {
    delta ^= Board::exact_deck_card_salt(zobrist, level, position,
                                         deck[position]);
  }
  board.cached_hash ^= delta;
  deck_order_hash_ ^= delta;
  return true;
}

bool RevealSearchState::matches_reference(
    const Board &board, const HiddenOutcomeCatalog &catalog) const noexcept {
  if (!active_)
    return true;
  std::array<CardIdSet, 3> expected_by_level{};
  CardIdSet expected_all;
  for (int level = 0; level < 3; ++level) {
    for (uint8_t card_id : board.decks[level]) {
      expected_by_level[level].add(card_id);
      expected_all.add(card_id);
    }
  }
  return expected_by_level == remaining_by_level_ &&
         expected_all == remaining_all_ &&
         catalog.acquired_hidden_cards(board) == acquired_hidden_ &&
         HiddenOutcomeCatalog::claimed_cards(board) == claimed_ &&
         board.hash_valid && board.cached_hash == board.compute_hash_uncached() &&
         deck_order_hash(board) == deck_order_hash_ &&
         board.compute_set_deck_search_hash() == rule_hash_;
}

void RevealSearchState::verify_or_abort(
    const Board &board, const HiddenOutcomeCatalog &catalog) const noexcept {
  if (!active_)
    return;
  CSPLENDOR_PERF_INC(SolverRevealStateOracleChecks);
  if (matches_reference(board, catalog))
    return;
  CSPLENDOR_PERF_INC(SolverRevealStateOracleFailures);
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
  const uint64_t exact_hash_oracle = board.compute_hash_uncached();
  const uint64_t deck_hash_oracle = deck_order_hash(board);
  const uint64_t rule_hash_oracle = board.compute_set_deck_search_hash();
  const bool exact_hash_matches =
      board.hash_valid && board.cached_hash == exact_hash_oracle;
  const bool deck_hash_matches = deck_hash_oracle == deck_order_hash_;
  const bool rule_hash_matches = rule_hash_oracle == rule_hash_;
  std::fprintf(stderr,
               "RevealSearchState oracle mismatch: exact_hash=%d "
               "deck_hash=%d rule_hash=%d cached=%llu exact=%llu "
               "deck=%llu deck_oracle=%llu rule=%llu rule_oracle=%llu "
               "turn=%u\n",
               exact_hash_matches, deck_hash_matches, rule_hash_matches,
               static_cast<unsigned long long>(board.cached_hash),
               static_cast<unsigned long long>(exact_hash_oracle),
               static_cast<unsigned long long>(deck_order_hash_),
               static_cast<unsigned long long>(deck_hash_oracle),
               static_cast<unsigned long long>(rule_hash_),
               static_cast<unsigned long long>(rule_hash_oracle),
               static_cast<unsigned>(board.turn));
#endif
  std::abort();
}

void RevealSearchState::deactivate() noexcept {
  for (CardIdSet &cards : remaining_by_level_)
    cards.clear();
  remaining_all_.clear();
  acquired_hidden_.clear();
  claimed_.clear();
  rule_hash_ = 0;
  deck_order_hash_ = 0;
  active_ = false;
}

void RevealSearchState::runtime_fallback(const char *reason) noexcept {
  CSPLENDOR_PERF_INC(SolverRevealStateRuntimeFallbacks);
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
  CSPLENDOR_PERF_INC(SolverRevealStateOracleFailures);
  std::fprintf(stderr, "RevealSearchState runtime fallback: %s\n", reason);
  std::abort();
#else
  (void)reason;
  deactivate();
#endif
}

bool OracleActionMetadata::less_than(
    const OracleActionMetadata &other) const noexcept {
  return std::tie(oracle_card, oracle_reserve, oracle_reserve_card,
                  oracle_return_color, oracle_gold_as) <
         std::tie(other.oracle_card, other.oracle_reserve,
                  other.oracle_reserve_card, other.oracle_return_color,
                  other.oracle_gold_as);
}

bool OracleActionMetadata::is_oracle() const noexcept {
  return oracle_card >= 0 || oracle_reserve;
}

RevealProofDagBuilder::RevealProofDagBuilder(size_t node_limit,
                                             size_t edge_limit) noexcept
    : node_limit_(node_limit), edge_limit_(edge_limit) {}

void RevealProofDagBuilder::reset() noexcept {
  edge_count_ = 0;
  nodes_.clear();
}

size_t RevealProofDagBuilder::node_count() const noexcept {
  return nodes_.size();
}

const std::vector<RevealVerifiedProofNode> &
RevealProofDagBuilder::nodes() const noexcept {
  return nodes_;
}

std::vector<RevealVerifiedProofNode> &RevealProofDagBuilder::nodes() noexcept {
  return nodes_;
}

size_t RevealProofDagBuilder::append_node(RevealVerifiedProofNode node) {
  if (node_limit_ && nodes_.size() >= node_limit_)
    throw ProofDagBuildAborted("proof DAG node limit exceeded");
  const size_t id = nodes_.size();
  nodes_.push_back(std::move(node));
  return id;
}

void RevealProofDagBuilder::append_edge(size_t parent,
                                        RevealVerifiedProofEdge edge) {
  if (edge_limit_ && edge_count_ >= edge_limit_)
    throw ProofDagBuildAborted("proof DAG edge limit exceeded");
  if (parent >= nodes_.size())
    throw ProofDagBuildAborted("proof DAG parent node is missing");
  nodes_[parent].children.push_back(std::move(edge));
  ++edge_count_;
}

std::vector<RevealVerifiedProofNode>
RevealProofDagBuilder::release_nodes() noexcept {
  edge_count_ = 0;
  std::vector<RevealVerifiedProofNode> released = std::move(nodes_);
  nodes_.clear();
  return released;
}

} // namespace csplendor::solver_internal
