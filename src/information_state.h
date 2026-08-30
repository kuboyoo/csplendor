#ifndef CSPLENDOR_INFORMATION_STATE_H
#define CSPLENDOR_INFORMATION_STATE_H

#include "game.h"
#include <array>
#include <cstdint>
#include <string>

namespace csplendor::information_state {

// Durable, observer-safe identity used by offline analysis and opening books.
// The bytes are an identity only: they deliberately omit the authoritative
// hidden partition and therefore cannot be deserialized into a Game.
static constexpr uint16_t FORMAT_VERSION = 1;
static constexpr uint16_t RULES_VERSION = 1;
static constexpr std::array<uint8_t, 8> MAGIC = {
    {'C', 'S', 'P', 'L', 'I', 'N', 'F', 'O'}};

std::string serialize(const Game &game, uint8_t observer);

// Fast persistent index for serialize(). Callers that require collision-free
// identity must retain and compare the serialized bytes as well.
uint64_t stable_hash(const Game &game, uint8_t observer);

} // namespace csplendor::information_state

#endif // CSPLENDOR_INFORMATION_STATE_H
