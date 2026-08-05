#include "state_invariants.h"
#include "resource_bundle.h"
#include <algorithm>
#include <array>
#include <sstream>

namespace csplendor::state {
namespace {

constexpr std::array<InvariantViolation, 22> ALL_VIOLATIONS = {
    InvariantViolation::InvalidCurrentPlayer,
    InvariantViolation::InvalidWinner,
    InvariantViolation::FixedCapacityOverflow,
    InvariantViolation::InvalidCardId,
    InvariantViolation::InvalidCardLevel,
    InvariantViolation::InvalidNobleId,
    InvariantViolation::InvalidProvenanceCapacity,
    InvariantViolation::PackedGemsMismatch,
    InvariantViolation::PackedBonusesMismatch,
    InvariantViolation::NobleEligibilityMismatch,
    InvariantViolation::ReservedCountMismatch,
    InvariantViolation::ReservedLayoutMismatch,
    InvariantViolation::HiddenReservationMismatch,
    InvariantViolation::PurchasedCountMismatch,
    InvariantViolation::BonusProvenanceMismatch,
    InvariantViolation::PointProvenanceMismatch,
    InvariantViolation::TokenConservationMismatch,
    InvariantViolation::PlayerTokenLimitExceeded,
    InvariantViolation::DuplicateCard,
    InvariantViolation::DuplicateNoble,
    InvariantViolation::NoblePartitionMismatch,
    InvariantViolation::StaleHashCache,
};

uint64_t pack_colours(const std::array<uint8_t, 5> &values) noexcept {
  uint64_t packed = 0;
  for (size_t colour = 0; colour < values.size(); ++colour)
    packed |= static_cast<uint64_t>(values[colour]) << (colour * 12U);
  return packed;
}

uint64_t pack_player_gems(const PlayerState &player) noexcept {
  uint64_t packed = 0;
  for (size_t colour = 0; colour < 5; ++colour)
    packed |= static_cast<uint64_t>(player.gems[colour]) << (colour * 12U);
  return packed;
}

uint16_t expected_noble_eligibility(uint64_t packed_bonuses) noexcept {
  uint16_t result = 0;
  for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
    const Noble &noble = get_noble(noble_id);
    if (cli::ResourceBundle::needed_gold(noble.packed_requirement,
                                         packed_bonuses, 0) == 0) {
      result |= static_cast<uint16_t>(1U << noble_id);
    }
  }
  return result;
}

bool strict_physical_profile(Profile profile) noexcept {
  return profile == Profile::Reachable || profile == Profile::Search;
}

void remember_card(std::array<uint8_t, CARD_COUNT> &seen, int card_id,
                   InvariantReport &report, bool check_duplicates) noexcept {
  if (!is_valid_card_id(card_id)) {
    report.add(InvariantViolation::InvalidCardId);
    return;
  }
  if (check_duplicates && seen[static_cast<size_t>(card_id)]++ != 0)
    report.add(InvariantViolation::DuplicateCard);
}

void remember_noble(std::array<uint8_t, NOBLE_COUNT> &seen, int noble_id,
                    InvariantReport &report, bool check_duplicates) noexcept {
  if (!is_valid_noble_id(noble_id)) {
    report.add(InvariantViolation::InvalidNobleId);
    return;
  }
  if (check_duplicates && seen[static_cast<size_t>(noble_id)]++ != 0)
    report.add(InvariantViolation::DuplicateNoble);
}

} // namespace

InvariantReport validate_invariants(const Board &board,
                                    Profile profile) noexcept {
  InvariantReport report;
  const bool strict = strict_physical_profile(profile);

  if (board.current_player >= Board::NUM_PLAYERS)
    report.add(InvariantViolation::InvalidCurrentPlayer);
  if (board.winner < -2 || board.winner > 1)
    report.add(InvariantViolation::InvalidWinner);

  bool fixed_capacities_safe = true;
  for (const auto &deck : board.decks) {
    if (deck.count > Board::MAX_DECK_SIZE) {
      report.add(InvariantViolation::FixedCapacityOverflow);
      fixed_capacities_safe = false;
    }
  }
  if (board.nobles.count > Board::MAX_NOBLES_ON_BOARD) {
    report.add(InvariantViolation::FixedCapacityOverflow);
    fixed_capacities_safe = false;
  }

  std::array<uint8_t, CARD_COUNT> seen_cards{};
  std::array<uint8_t, NOBLE_COUNT> seen_nobles{};

  for (int level = 0; level < 3; ++level) {
    for (int8_t card_id : board.visible[static_cast<size_t>(level)]) {
      if (card_id == -1)
        continue;
      remember_card(seen_cards, card_id, report, strict);
      if (strict && is_valid_card_id(card_id) &&
          get_card(card_id).level != level + 1) {
        report.add(InvariantViolation::InvalidCardLevel);
      }
    }

    const auto &deck = board.decks[level];
    const size_t safe_count =
        std::min<size_t>(deck.count, Board::MAX_DECK_SIZE);
    for (size_t slot = 0; slot < safe_count; ++slot) {
      const int card_id = deck.data[slot];
      remember_card(seen_cards, card_id, report, strict);
      if (strict && is_valid_card_id(card_id) &&
          get_card(card_id).level != level + 1) {
        report.add(InvariantViolation::InvalidCardLevel);
      }
    }
  }

  const size_t safe_noble_count =
      std::min<size_t>(board.nobles.count, Board::MAX_NOBLES_ON_BOARD);
  for (size_t slot = 0; slot < safe_noble_count; ++slot)
    remember_noble(seen_nobles, board.nobles.data[slot], report, strict);

  std::array<int, 6> token_totals{};
  for (size_t colour = 0; colour < token_totals.size(); ++colour)
    token_totals[colour] = board.bank[colour];

  size_t total_nobles = safe_noble_count;
  for (const PlayerState &player : board.players) {
    if (player.purchased_cards.size() > CARD_COUNT ||
        player.acquired_nobles.size() > NOBLE_COUNT) {
      report.add(InvariantViolation::InvalidProvenanceCapacity);
    }

    const uint64_t expected_gems = pack_player_gems(player);
    const uint64_t expected_bonuses = pack_colours(player.bonuses);
    if (player.packed_gems != expected_gems)
      report.add(InvariantViolation::PackedGemsMismatch);
    if (player.packed_bonuses != expected_bonuses)
      report.add(InvariantViolation::PackedBonusesMismatch);
    if (player.noble_eligibility_mask !=
        expected_noble_eligibility(expected_bonuses)) {
      report.add(InvariantViolation::NobleEligibilityMismatch);
    }

    uint8_t occupied_reserved = 0;
    bool found_empty_reserved = false;
    for (size_t slot = 0; slot < player.reserved.size(); ++slot) {
      const int card_id = player.reserved[slot];
      if (card_id == -1) {
        found_empty_reserved = true;
        if (strict && player.reserved_is_hidden[slot])
          report.add(InvariantViolation::HiddenReservationMismatch);
        continue;
      }
      ++occupied_reserved;
      if (strict && found_empty_reserved)
        report.add(InvariantViolation::ReservedLayoutMismatch);
      remember_card(seen_cards, card_id, report, strict);
    }
    if (player.reserved_count > Board::MAX_RESERVED ||
        player.reserved_count != occupied_reserved) {
      report.add(InvariantViolation::ReservedCountMismatch);
    }

    std::array<int, 5> expected_bonus_counts{};
    int expected_points = 0;
    for (uint8_t card_id : player.purchased_cards) {
      remember_card(seen_cards, card_id, report, strict);
      if (is_valid_card_id(card_id)) {
        const Card &card = get_card(card_id);
        ++expected_bonus_counts[static_cast<size_t>(card.bonus)];
        expected_points += card.points;
      }
    }
    if (strict && player.purchased_count != player.purchased_cards.size())
      report.add(InvariantViolation::PurchasedCountMismatch);
    if (strict) {
      for (size_t colour = 0; colour < expected_bonus_counts.size(); ++colour) {
        if (player.bonuses[colour] != expected_bonus_counts[colour]) {
          report.add(InvariantViolation::BonusProvenanceMismatch);
          break;
        }
      }
    }

    for (uint8_t noble_id : player.acquired_nobles) {
      remember_noble(seen_nobles, noble_id, report, strict);
      if (is_valid_noble_id(noble_id))
        expected_points += get_noble(noble_id).points;
    }
    total_nobles += player.acquired_nobles.size();
    if (strict && player.points != expected_points)
      report.add(InvariantViolation::PointProvenanceMismatch);

    if (strict && player.total_gems() > Board::MAX_TOKENS)
      report.add(InvariantViolation::PlayerTokenLimitExceeded);
    for (size_t colour = 0; colour < token_totals.size(); ++colour)
      token_totals[colour] += player.gems[colour];
  }

  if (strict) {
    constexpr std::array<int, 6> EXPECTED_TOKENS = {
        Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR,
        Board::GEMS_PER_COLOR, Board::GEMS_PER_COLOR, Board::NUM_GOLD};
    if (token_totals != EXPECTED_TOKENS)
      report.add(InvariantViolation::TokenConservationMismatch);
    if (total_nobles != Board::NUM_NOBLES)
      report.add(InvariantViolation::NoblePartitionMismatch);
  }

  // compute_hash_uncached() iterates FixedStack::size(), so only use it as an
  // oracle after establishing that externally writable counts are in range.
  if (fixed_capacities_safe && board.hash_valid &&
      board.cached_hash != board.compute_hash_uncached()) {
    report.add(InvariantViolation::StaleHashCache);
  }

  return report;
}

const char *invariant_violation_name(InvariantViolation violation) noexcept {
  switch (violation) {
  case InvariantViolation::InvalidCurrentPlayer:
    return "invalid_current_player";
  case InvariantViolation::InvalidWinner:
    return "invalid_winner";
  case InvariantViolation::FixedCapacityOverflow:
    return "fixed_capacity_overflow";
  case InvariantViolation::InvalidCardId:
    return "invalid_card_id";
  case InvariantViolation::InvalidCardLevel:
    return "invalid_card_level";
  case InvariantViolation::InvalidNobleId:
    return "invalid_noble_id";
  case InvariantViolation::InvalidProvenanceCapacity:
    return "invalid_provenance_capacity";
  case InvariantViolation::PackedGemsMismatch:
    return "packed_gems_mismatch";
  case InvariantViolation::PackedBonusesMismatch:
    return "packed_bonuses_mismatch";
  case InvariantViolation::NobleEligibilityMismatch:
    return "noble_eligibility_mismatch";
  case InvariantViolation::ReservedCountMismatch:
    return "reserved_count_mismatch";
  case InvariantViolation::ReservedLayoutMismatch:
    return "reserved_layout_mismatch";
  case InvariantViolation::HiddenReservationMismatch:
    return "hidden_reservation_mismatch";
  case InvariantViolation::PurchasedCountMismatch:
    return "purchased_count_mismatch";
  case InvariantViolation::BonusProvenanceMismatch:
    return "bonus_provenance_mismatch";
  case InvariantViolation::PointProvenanceMismatch:
    return "point_provenance_mismatch";
  case InvariantViolation::TokenConservationMismatch:
    return "token_conservation_mismatch";
  case InvariantViolation::PlayerTokenLimitExceeded:
    return "player_token_limit_exceeded";
  case InvariantViolation::DuplicateCard:
    return "duplicate_card";
  case InvariantViolation::DuplicateNoble:
    return "duplicate_noble";
  case InvariantViolation::NoblePartitionMismatch:
    return "noble_partition_mismatch";
  case InvariantViolation::StaleHashCache:
    return "stale_hash_cache";
  }
  return "unknown_invariant_violation";
}

std::string describe_invariant_violations(const InvariantReport &report) {
  if (report.ok())
    return "ok";

  std::ostringstream description;
  bool first = true;
  for (InvariantViolation violation : ALL_VIOLATIONS) {
    if (!report.has(violation))
      continue;
    if (!first)
      description << ',';
    description << invariant_violation_name(violation);
    first = false;
  }
  return description.str();
}

} // namespace csplendor::state
