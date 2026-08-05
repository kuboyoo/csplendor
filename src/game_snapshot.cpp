#include "game_snapshot.h"
#include "card_data.h"
#include "noble_data.h"
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace csplendor::snapshot {

void Writer::u8(uint8_t value) { data_.push_back(static_cast<char>(value)); }

void Writer::i8(int8_t value) { u8(static_cast<uint8_t>(value)); }

void Writer::u16(uint16_t value) {
  u8(static_cast<uint8_t>(value & 0xffU));
  u8(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

void Writer::u32(uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    u8(static_cast<uint8_t>((value >> shift) & 0xffU));
}

void Writer::u64(uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    u8(static_cast<uint8_t>((value >> shift) & 0xffU));
}

void Writer::bytes(std::string_view value) { data_.append(value); }

const std::string &Writer::data() const { return data_; }

std::string Writer::take() { return std::move(data_); }

Reader::Reader(std::string_view data) : data_(data) {}

uint8_t Reader::u8() {
  require(1);
  return static_cast<uint8_t>(data_[offset_++]);
}

int8_t Reader::i8() {
  const int value = static_cast<int>(u8());
  return static_cast<int8_t>(value < 128 ? value : value - 256);
}

uint16_t Reader::u16() {
  uint16_t value = 0;
  for (unsigned shift = 0; shift < 16; shift += 8)
    value |= static_cast<uint16_t>(u8()) << shift;
  return value;
}

uint32_t Reader::u32() {
  uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8)
    value |= static_cast<uint32_t>(u8()) << shift;
  return value;
}

uint64_t Reader::u64() {
  uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8)
    value |= static_cast<uint64_t>(u8()) << shift;
  return value;
}

std::string_view Reader::bytes(size_t size) {
  require(size);
  const std::string_view result = data_.substr(offset_, size);
  offset_ += size;
  return result;
}

size_t Reader::remaining() const { return data_.size() - offset_; }

void Reader::require(size_t size) const {
  if (size > data_.size() - offset_)
    throw std::invalid_argument("truncated csplendor game snapshot");
}

uint64_t fingerprint_bytes(uint64_t hash, uint8_t value) {
  hash ^= static_cast<uint64_t>(value);
  return hash * 1099511628211ULL;
}

uint64_t ruleset_fingerprint() {
  static const uint64_t fingerprint = [] {
    uint64_t hash = 14695981039346656037ULL;
    hash = fingerprint_bytes(hash, static_cast<uint8_t>(CARD_COUNT));
    hash = fingerprint_bytes(hash, static_cast<uint8_t>(NOBLE_COUNT));
    for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
      const Card &card = get_card(card_id);
      hash = fingerprint_bytes(hash, card.id);
      hash = fingerprint_bytes(hash, card.points);
      for (uint8_t cost : card.cost)
        hash = fingerprint_bytes(hash, cost);
      hash = fingerprint_bytes(hash, static_cast<uint8_t>(card.bonus));
      hash = fingerprint_bytes(hash, card.level);
    }
    for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
      const Noble &noble = get_noble(noble_id);
      hash = fingerprint_bytes(hash, noble.id);
      hash = fingerprint_bytes(hash, noble.points);
      for (uint8_t requirement : noble.requirement)
        hash = fingerprint_bytes(hash, requirement);
    }
    return hash;
  }();
  return fingerprint;
}

uint64_t payload_checksum(std::string_view payload) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char value : payload)
    hash = fingerprint_bytes(hash, value);
  return hash;
}

bool valid_card_id(int value) { return value >= 0 && value < CARD_COUNT; }

bool valid_noble_id(int value) { return value >= 0 && value < NOBLE_COUNT; }

void require_boolean(uint8_t value, const char *field) {
  if (value > 1)
    throw std::invalid_argument(
        std::string("invalid boolean in game snapshot: ") + field);
}

std::string serialize(const Game &game) {
  Writer payload;
  uint8_t mode_flags = 0;
  mode_flags |= static_cast<uint8_t>(game.simple_payment_mode);
  mode_flags |= static_cast<uint8_t>(game.blank_refill_mode) << 1U;
  payload.u8(mode_flags);

  const Board &board = game.board;
  if (board.current_player >= Board::NUM_PLAYERS || board.winner < -2 ||
      board.winner > 1)
    throw std::invalid_argument("invalid game status for game snapshot");
  payload.u8(board.current_player);
  payload.u16(board.turn);
  payload.u8(static_cast<uint8_t>(board.final_round));
  payload.u8(static_cast<uint8_t>(board.waiting_noble));
  payload.i8(board.winner);

  for (uint8_t value : board.bank)
    payload.u8(value);
  for (const auto &level : board.visible) {
    for (int8_t card_id : level) {
      if (card_id != -1 && !valid_card_id(card_id))
        throw std::invalid_argument("invalid visible card for game snapshot");
      payload.i8(card_id);
    }
  }

  for (const auto &deck : board.decks) {
    payload.u8(static_cast<uint8_t>(deck.size()));
    for (uint8_t card_id : deck) {
      if (!valid_card_id(card_id))
        throw std::invalid_argument("invalid deck card for game snapshot");
      payload.u8(card_id);
    }
  }

  payload.u8(static_cast<uint8_t>(board.nobles.size()));
  for (uint8_t noble_id : board.nobles) {
    if (!valid_noble_id(noble_id))
      throw std::invalid_argument("invalid noble for game snapshot");
    payload.u8(noble_id);
  }

  for (const PlayerState &player : board.players) {
    for (uint8_t value : player.gems)
      payload.u8(value);
    for (uint8_t value : player.bonuses)
      payload.u8(value);
    payload.u8(player.points);
    for (int8_t card_id : player.reserved) {
      if (card_id != -1 && !valid_card_id(card_id))
        throw std::invalid_argument("invalid reserved card for game snapshot");
      payload.i8(card_id);
    }
    for (bool hidden : player.reserved_is_hidden)
      payload.u8(static_cast<uint8_t>(hidden));
    payload.u8(player.reserved_count);
    payload.u8(player.purchased_count);

    if (player.reserved_count > Board::MAX_RESERVED ||
        player.purchased_count > CARD_COUNT ||
        player.purchased_cards.size() > CARD_COUNT)
      throw std::invalid_argument("invalid player count for game snapshot");
    payload.u8(static_cast<uint8_t>(player.purchased_cards.size()));
    for (uint8_t card_id : player.purchased_cards) {
      if (!valid_card_id(card_id))
        throw std::invalid_argument("invalid purchased card for game snapshot");
      payload.u8(card_id);
    }

    if (player.acquired_nobles.size() > NOBLE_COUNT)
      throw std::invalid_argument("too many acquired nobles in game snapshot");
    payload.u8(static_cast<uint8_t>(player.acquired_nobles.size()));
    for (uint8_t noble_id : player.acquired_nobles) {
      if (!valid_noble_id(noble_id))
        throw std::invalid_argument("invalid acquired noble for game snapshot");
      payload.u8(noble_id);
    }
  }

  if (payload.data().size() > GAME_SNAPSHOT_MAX_PAYLOAD_BYTES)
    throw std::length_error("csplendor game snapshot payload is too large");

  Writer snapshot;
  snapshot.bytes(std::string_view(
      reinterpret_cast<const char *>(GAME_SNAPSHOT_MAGIC.data()),
      GAME_SNAPSHOT_MAGIC.size()));
  snapshot.u16(GAME_SNAPSHOT_FORMAT_VERSION);
  snapshot.u16(GAME_SNAPSHOT_RULES_VERSION);
  snapshot.u16(static_cast<uint16_t>(CARD_COUNT));
  snapshot.u16(static_cast<uint16_t>(NOBLE_COUNT));
  snapshot.u64(ruleset_fingerprint());
  snapshot.u32(static_cast<uint32_t>(payload.data().size()));
  snapshot.bytes(payload.data());
  snapshot.u64(payload_checksum(payload.data()));
  return snapshot.take();
}

Game deserialize(std::string_view snapshot) {
  if (snapshot.size() > GAME_SNAPSHOT_MAX_BYTES)
    throw std::invalid_argument("csplendor game snapshot is too large");

  Reader envelope(snapshot);
  const std::string_view magic = envelope.bytes(GAME_SNAPSHOT_MAGIC.size());
  const std::string_view expected_magic(
      reinterpret_cast<const char *>(GAME_SNAPSHOT_MAGIC.data()),
      GAME_SNAPSHOT_MAGIC.size());
  if (magic != expected_magic)
    throw std::invalid_argument("invalid csplendor game snapshot magic");

  const uint16_t version = envelope.u16();
  if (version != GAME_SNAPSHOT_FORMAT_VERSION)
    throw std::invalid_argument("unsupported csplendor game snapshot version");
  if (envelope.u16() != GAME_SNAPSHOT_RULES_VERSION)
    throw std::invalid_argument(
        "csplendor game snapshot rules version mismatch");
  if (envelope.u16() != CARD_COUNT || envelope.u16() != NOBLE_COUNT ||
      envelope.u64() != ruleset_fingerprint())
    throw std::invalid_argument("csplendor game snapshot ruleset mismatch");

  const uint32_t payload_size = envelope.u32();
  if (payload_size > GAME_SNAPSHOT_MAX_PAYLOAD_BYTES ||
      envelope.remaining() !=
          static_cast<size_t>(payload_size) + sizeof(uint64_t))
    throw std::invalid_argument("invalid csplendor game snapshot length");
  const std::string_view payload = envelope.bytes(payload_size);
  if (envelope.u64() != payload_checksum(payload) || envelope.remaining() != 0)
    throw std::invalid_argument("csplendor game snapshot checksum mismatch");

  Reader reader(payload);
  const uint8_t mode_flags = reader.u8();
  if (mode_flags & ~uint8_t{3})
    throw std::invalid_argument("invalid csplendor game snapshot mode flags");

  Board board;
  board.current_player = reader.u8();
  board.turn = reader.u16();
  const uint8_t final_round = reader.u8();
  const uint8_t waiting_noble = reader.u8();
  require_boolean(final_round, "final_round");
  require_boolean(waiting_noble, "waiting_noble");
  board.final_round = final_round != 0;
  board.waiting_noble = waiting_noble != 0;
  board.winner = reader.i8();
  if (board.current_player >= Board::NUM_PLAYERS || board.winner < -2 ||
      board.winner > 1)
    throw std::invalid_argument("invalid game status in game snapshot");

  for (uint8_t &value : board.bank)
    value = reader.u8();
  for (auto &level : board.visible) {
    for (int8_t &card_id : level) {
      card_id = reader.i8();
      if (card_id != -1 && !valid_card_id(card_id))
        throw std::invalid_argument("invalid visible card in game snapshot");
    }
  }

  for (auto &deck : board.decks) {
    deck.clear();
    const uint8_t count = reader.u8();
    if (count > Board::MAX_DECK_SIZE)
      throw std::invalid_argument("invalid deck size in game snapshot");
    for (uint8_t index = 0; index < count; ++index) {
      const uint8_t card_id = reader.u8();
      if (!valid_card_id(card_id))
        throw std::invalid_argument("invalid deck card in game snapshot");
      deck.push_back(card_id);
    }
  }

  board.nobles.clear();
  const uint8_t noble_count = reader.u8();
  if (noble_count > NOBLE_COUNT)
    throw std::invalid_argument("invalid noble count in game snapshot");
  for (uint8_t index = 0; index < noble_count; ++index) {
    const uint8_t noble_id = reader.u8();
    if (!valid_noble_id(noble_id))
      throw std::invalid_argument("invalid noble in game snapshot");
    if (!board.nobles.try_push_back(noble_id))
      throw std::invalid_argument("invalid noble count in game snapshot");
  }

  for (PlayerState &player : board.players) {
    for (uint8_t &value : player.gems)
      value = reader.u8();
    for (uint8_t &value : player.bonuses)
      value = reader.u8();
    player.points = reader.u8();
    for (int8_t &card_id : player.reserved) {
      card_id = reader.i8();
      if (card_id != -1 && !valid_card_id(card_id))
        throw std::invalid_argument("invalid reserved card in game snapshot");
    }
    for (bool &hidden : player.reserved_is_hidden) {
      const uint8_t value = reader.u8();
      require_boolean(value, "reserved_is_hidden");
      hidden = value != 0;
    }
    player.reserved_count = reader.u8();
    player.purchased_count = reader.u8();
    if (player.reserved_count > Board::MAX_RESERVED ||
        player.purchased_count > CARD_COUNT)
      throw std::invalid_argument("invalid player count in game snapshot");

    const uint8_t purchased_size = reader.u8();
    if (purchased_size > CARD_COUNT)
      throw std::invalid_argument(
          "invalid purchased-card list in game snapshot");
    player.purchased_cards.clear();
    player.purchased_cards.reserve(purchased_size);
    for (uint8_t index = 0; index < purchased_size; ++index) {
      const uint8_t card_id = reader.u8();
      if (!valid_card_id(card_id))
        throw std::invalid_argument("invalid purchased card in game snapshot");
      player.purchased_cards.push_back(card_id);
    }

    const uint8_t acquired_noble_size = reader.u8();
    if (acquired_noble_size > NOBLE_COUNT)
      throw std::invalid_argument(
          "invalid acquired-noble list in game snapshot");
    player.acquired_nobles.clear();
    player.acquired_nobles.reserve(acquired_noble_size);
    for (uint8_t index = 0; index < acquired_noble_size; ++index) {
      const uint8_t noble_id = reader.u8();
      if (!valid_noble_id(noble_id))
        throw std::invalid_argument("invalid acquired noble in game snapshot");
      player.acquired_nobles.push_back(noble_id);
    }
    player.sync_packed();
  }

  if (reader.remaining() != 0)
    throw std::invalid_argument("trailing data in csplendor game snapshot");
  board.begin_editor_mutation();

  Game game(0);
  game.board = std::move(board);
  game.simple_payment_mode = (mode_flags & 1U) != 0;
  game.blank_refill_mode = (mode_flags & 2U) != 0;
  return game;
}

} // namespace csplendor::snapshot
