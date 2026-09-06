#include "information_state.h"
#include "card_data.h"
#include "game_snapshot.h"
#include "noble_data.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace csplendor::information_state {
namespace {

struct ObservedReservation {
  // 0: publicly known card, 1: observer's private card,
  // 2: opponent's hidden card represented only by its public tier.
  uint8_t kind = 0;
  uint8_t value = 0;

  bool operator<(const ObservedReservation &other) const {
    return std::tie(kind, value) < std::tie(other.kind, other.value);
  }
};

void require_card(int card_id, const char *field) {
  if (!snapshot::valid_card_id(card_id))
    throw std::invalid_argument(std::string("invalid card in ") + field);
}

void require_noble(int noble_id, const char *field) {
  if (!snapshot::valid_noble_id(noble_id))
    throw std::invalid_argument(std::string("invalid noble in ") + field);
}

template <typename Values>
std::vector<uint8_t> sorted_ids(const Values &values, bool nobles,
                                const char *field) {
  std::vector<uint8_t> result;
  result.reserve(values.size());
  for (const auto raw_value : values) {
    const int value = static_cast<int>(raw_value);
    if (nobles)
      require_noble(value, field);
    else
      require_card(value, field);
    result.push_back(static_cast<uint8_t>(value));
  }
  std::sort(result.begin(), result.end());
  return result;
}

void append_counted(snapshot::Writer &writer,
                    const std::vector<uint8_t> &values) {
  if (values.size() > 255)
    throw std::length_error("information-state list is too large");
  writer.u8(static_cast<uint8_t>(values.size()));
  for (const uint8_t value : values)
    writer.u8(value);
}

std::vector<ObservedReservation>
observed_reservations(const Board &board, uint8_t player_id,
                      uint8_t observer) {
  const PlayerState &player = board.players[player_id];
  std::vector<ObservedReservation> reservations;
  reservations.reserve(Board::MAX_RESERVED);
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
    const int card_id = player.reserved[slot];
    if (card_id < 0)
      continue;
    require_card(card_id, "information-state reservation");
    if (player_id != observer && player.reserved_is_hidden[slot]) {
      const int level = get_card(card_id).level;
      if (level < 1 || level > 3)
        throw std::invalid_argument(
            "invalid hidden reservation tier in information state");
      reservations.push_back(
          {2, static_cast<uint8_t>(level)});
    } else {
      reservations.push_back(
          {static_cast<uint8_t>(player.reserved_is_hidden[slot] ? 1 : 0),
           static_cast<uint8_t>(card_id)});
    }
  }
  if (reservations.size() != player.reserved_count)
    throw std::invalid_argument(
        "reserved count does not match information-state cards");
  std::sort(reservations.begin(), reservations.end());
  return reservations;
}

} // namespace

std::string serialize(const Game &game, uint8_t observer) {
  if (observer >= Board::NUM_PLAYERS)
    throw std::invalid_argument("observer must identify a player");

  const Board &board = game.board;
  if (board.current_player >= Board::NUM_PLAYERS || board.winner < -2 ||
      board.winner > 1)
    throw std::invalid_argument("invalid game status for information state");

  snapshot::Writer payload;
  payload.u8(observer);
  uint8_t mode_flags = 0;
  mode_flags |= static_cast<uint8_t>(game.simple_payment_mode);
  mode_flags |= static_cast<uint8_t>(game.blank_refill_mode) << 1U;
  payload.u8(mode_flags);
  payload.u8(board.current_player);
  payload.u16(board.turn);
  payload.u8(static_cast<uint8_t>(board.final_round));
  payload.u8(static_cast<uint8_t>(board.waiting_noble));
  payload.i8(board.winner);

  for (const uint8_t value : board.bank)
    payload.u8(value);

  // Slot order has no rule meaning: actions address a card by ID, and a
  // refill is canonicalized again after it becomes public.
  for (int level = 0; level < 3; ++level) {
    std::vector<uint8_t> visible;
    visible.reserve(Board::CARDS_PER_LEVEL);
    for (const int8_t card_id : board.visible[level]) {
      if (card_id < 0)
        continue;
      require_card(card_id, "information-state visible cards");
      if (get_card(card_id).level != level + 1)
        throw std::invalid_argument(
            "visible card tier does not match information state");
      visible.push_back(static_cast<uint8_t>(card_id));
    }
    std::sort(visible.begin(), visible.end());
    append_counted(payload, visible);

    if (board.decks[level].size() > Board::MAX_DECK_SIZE)
      throw std::invalid_argument(
          "invalid deck size for information state");
    payload.u8(static_cast<uint8_t>(board.decks[level].size()));
    const std::vector<uint8_t> pool =
        board.observable_card_pool(observer, level + 1);
    for (const uint8_t card_id : pool) {
      require_card(card_id, "information-state observable pool");
      if (get_card(card_id).level != level + 1)
        throw std::invalid_argument(
            "observable pool card tier does not match information state");
    }
    append_counted(payload, pool);
  }

  const std::vector<uint8_t> nobles =
      sorted_ids(board.nobles, true, "information-state nobles");
  append_counted(payload, nobles);

  for (uint8_t player_id = 0; player_id < Board::NUM_PLAYERS; ++player_id) {
    const PlayerState &player = board.players[player_id];
    for (const uint8_t value : player.gems)
      payload.u8(value);
    for (const uint8_t value : player.bonuses)
      payload.u8(value);
    payload.u8(player.points);
    payload.u8(player.reserved_count);
    payload.u8(player.purchased_count);

    const auto reservations =
        observed_reservations(board, player_id, observer);
    payload.u8(static_cast<uint8_t>(reservations.size()));
    for (const ObservedReservation &reservation : reservations) {
      payload.u8(reservation.kind);
      payload.u8(reservation.value);
    }

    if (player.purchased_cards.size() > player.purchased_count)
      throw std::invalid_argument(
          "known purchased cards exceed information-state purchase count");
    const std::vector<uint8_t> purchased = sorted_ids(
        player.purchased_cards, false, "information-state purchased cards");
    append_counted(payload, purchased);
    payload.u8(static_cast<uint8_t>(player.purchased_count - purchased.size()));

    const std::vector<uint8_t> acquired = sorted_ids(
        player.acquired_nobles, true, "information-state acquired nobles");
    append_counted(payload, acquired);
  }

  snapshot::Writer envelope;
  envelope.bytes(std::string_view(reinterpret_cast<const char *>(MAGIC.data()),
                                  MAGIC.size()));
  envelope.u16(FORMAT_VERSION);
  envelope.u16(RULES_VERSION);
  envelope.u16(static_cast<uint16_t>(CARD_COUNT));
  envelope.u16(static_cast<uint16_t>(NOBLE_COUNT));
  envelope.u64(snapshot::ruleset_fingerprint());
  envelope.u32(static_cast<uint32_t>(payload.data().size()));
  envelope.bytes(payload.data());
  envelope.u64(snapshot::payload_checksum(payload.data()));
  return envelope.take();
}

uint64_t stable_hash(const Game &game, uint8_t observer) {
  const std::string serialized = serialize(game, observer);
  return snapshot::payload_checksum(serialized);
}

} // namespace csplendor::information_state
