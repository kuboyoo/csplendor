#ifndef CSPLENDOR_GAME_SNAPSHOT_H
#define CSPLENDOR_GAME_SNAPSHOT_H

#include "game.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace csplendor::snapshot {

// Version 5: no reservation carries a token return (the excess is returned
// by RETURN_GEM after the refill or the drawn card is seen); the payload has
// the pending return, the end reason and the repetition window. Older
// versions are rejected.
static constexpr uint16_t GAME_SNAPSHOT_FORMAT_VERSION = 5;
static constexpr uint16_t GAME_SNAPSHOT_RULES_VERSION = 5;
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
// Accepts the current version only.
Game deserialize(std::string_view snapshot);

} // namespace csplendor::snapshot

#endif // CSPLENDOR_GAME_SNAPSHOT_H
