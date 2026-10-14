#ifndef CSPLENDOR_GAME_SNAPSHOT_H
#define CSPLENDOR_GAME_SNAPSHOT_H

#include "game.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace csplendor::snapshot {

// Version 2 adds the pending deck-reservation return (Board::waiting_return)
// and the rules in which a deck reservation never carries a token return.
// Version 3 adds the passive streak and the end reason; version 4 replaces
// the streak with the repetition window (a position occurring for the fourth
// time draws).
static constexpr uint16_t GAME_SNAPSHOT_FORMAT_VERSION = 4;
static constexpr uint16_t GAME_SNAPSHOT_RULES_VERSION = 4;
static constexpr std::array<uint8_t, 8> GAME_SNAPSHOT_MAGIC = {
    {'C', 'S', 'P', 'L', 'S', 'N', 'P', '\0'}};
// The repetition window takes eight bytes per position of the current
// token-only stretch of play.
static constexpr size_t GAME_SNAPSHOT_MAX_BYTES = 65536;
static constexpr size_t GAME_SNAPSHOT_MAX_PAYLOAD_BYTES = 65440;

class Writer {
public:
  void u8(uint8_t value);
  void i8(int8_t value);
  void u16(uint16_t value);
  void u32(uint32_t value);
  void u64(uint64_t value);
  void bytes(std::string_view value);

  const std::string &data() const;
  std::string take();

private:
  std::string data_;
};

class Reader {
public:
  explicit Reader(std::string_view data);

  uint8_t u8();
  int8_t i8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  std::string_view bytes(size_t size);
  size_t remaining() const;

private:
  void require(size_t size) const;

  std::string_view data_;
  size_t offset_ = 0;
};

uint64_t fingerprint_bytes(uint64_t hash, uint8_t value);
uint64_t ruleset_fingerprint();
uint64_t payload_checksum(std::string_view payload);
bool valid_card_id(int value);
bool valid_noble_id(int value);
void require_boolean(uint8_t value, const char *field);

std::string serialize(const Game &game);
// Accepts the current version and, so that stored self-play stays usable,
// versions 2 (before the repetition rule) and 3 (passive streak). Their
// positions start a repetition window.
Game deserialize(std::string_view snapshot);
// One-off migration: re-encodes a format/rules version 1 snapshot as the
// current version. Version 1 positions never have a pending return.
std::string upgrade_v1(std::string_view snapshot);

} // namespace csplendor::snapshot

#endif // CSPLENDOR_GAME_SNAPSHOT_H
