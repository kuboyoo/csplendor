#include "game_snapshot.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_public_header_roundtrip_and_corruption_rejection() {
  Game game(42);
  for (int ply = 0; ply < 18; ++ply) {
    const auto codes = game.legal_action_codes();
    if (codes.empty())
      break;
    const auto index = static_cast<std::size_t>((ply * 7 + 3) % codes.size());
    require(game.apply_action_code_trusted(codes[index], true),
            "failed to build the snapshot fixture");
  }

  const std::string encoded = csplendor::snapshot::serialize(game);
  const Game restored = csplendor::snapshot::deserialize(encoded);
  require(csplendor::snapshot::serialize(restored) == encoded,
          "snapshot bytes changed after roundtrip");
  require(restored.board.hash() == game.board.hash(),
          "snapshot roundtrip changed the board hash");
  require(restored.legal_action_codes() == game.legal_action_codes(),
          "snapshot roundtrip changed legal action order");

  std::string corrupted = encoded;
  corrupted[corrupted.size() / 2] ^= 1;
  try {
    (void)csplendor::snapshot::deserialize(corrupted);
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error("corrupted snapshot was accepted");
}

} // namespace

int main() {
  static_assert(csplendor::snapshot::GAME_SNAPSHOT_FORMAT_VERSION == 1,
                "snapshot format version changed");
  static_assert(csplendor::snapshot::GAME_SNAPSHOT_RULES_VERSION == 1,
                "snapshot rules version changed");
  try {
    test_public_header_roundtrip_and_corruption_rejection();
  } catch (const std::exception &error) {
    std::cerr << "game_snapshot_unit: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
