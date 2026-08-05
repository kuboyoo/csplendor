#include "action_encoder.h"
#include "action_encoder_v2.h"
#include "action_encoder_v3.h"
#include "encoding_schema.h"
#include "state_encoder.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

void check(bool condition) {
  if (!condition)
    std::abort();
}

template <typename Integer>
void digest_little_endian(uint64_t &digest, Integer value, size_t bytes) {
  const uint64_t widened = static_cast<uint64_t>(value);
  for (size_t index = 0; index < bytes; ++index) {
    digest ^= static_cast<uint8_t>(widened >> (index * 8));
    digest *= FNV_PRIME;
  }
}

template <typename Schema> void check_action_sections() {
  uint32_t expected_offset = 0;
  for (const auto &section : Schema::SECTIONS) {
    check(section.offset == expected_offset);
    check(section.size > 0);
    expected_offset += section.size;
  }
  check(expected_offset == static_cast<uint32_t>(Schema::SIZE));
}

template <typename Encoder>
uint64_t decoded_action_digest(const Game &game, int size) {
  uint64_t digest = FNV_OFFSET;
  for (int action_id = 0; action_id < size; ++action_id) {
    digest_little_endian(digest, Encoder::decode(action_id, game).pack(), 8);
  }
  return digest;
}

void test_action_schemas_and_full_id_golden() {
  using csplendor::encoding::ActionSpaceV1;
  using csplendor::encoding::ActionSpaceV2;
  using csplendor::encoding::ActionSpaceV3;

  static_assert(ActionSpaceV1::SIZE == 48);
  static_assert(ActionSpaceV2::SIZE == 4869);
  static_assert(ActionSpaceV3::SIZE == 3133);
  static_assert(ActionEncoderCpp::BASE_ACTION_COUNT == ActionSpaceV1::SIZE);
  static_assert(ActionEncoderV2::ACTION_SIZE == ActionSpaceV2::SIZE);
  static_assert(ActionEncoderV3::ACTION_SIZE == ActionSpaceV3::SIZE);
  static_assert(ActionEncoderV2::OFFSET_PASS == ActionSpaceV2::OFFSET_PASS);
  static_assert(ActionEncoderV3::OFFSET_PURCHASE ==
                ActionSpaceV3::OFFSET_PURCHASE);

  check_action_sections<ActionSpaceV1>();
  check_action_sections<ActionSpaceV2>();
  check_action_sections<ActionSpaceV3>();

  check(std::string_view(ActionSpaceV1::fingerprint()) ==
        "csplendor.action.v1;size=48;layout=10,5,12,3,12,3,3");
  check(std::string_view(ActionSpaceV2::fingerprint()) ==
        "csplendor.action.v2;size=4869;layout="
        "840,140,84,21,3024,756,3,1");
  check(std::string_view(ActionSpaceV3::fingerprint()) ==
        "csplendor.action.v3;size=3133;layout="
        "840,140,84,21,2035,12,1");

  const Game game(42);
  check(decoded_action_digest<ActionEncoderCpp>(game, ActionSpaceV1::SIZE) ==
        0x2b6c6bfb7226c44dULL);
  check(decoded_action_digest<ActionEncoderV2>(game, ActionSpaceV2::SIZE) ==
        0xb076fc64ccd1e74aULL);
  check(decoded_action_digest<ActionEncoderV3>(game, ActionSpaceV3::SIZE) ==
        0xf3c8519bf281d5f3ULL);
}

void test_state_schema_and_reachable_golden() {
  using Schema = csplendor::encoding::StateFeatureV1;
  static_assert(Schema::SIZE == 196);
  static_assert(Schema::OFFSET_PLAYER_0 == 6);
  static_assert(Schema::OFFSET_PLAYER_1 == 42);
  static_assert(Schema::OFFSET_VISIBLE_CARDS == 78);
  static_assert(Schema::OFFSET_DECK_COUNTS == 174);
  static_assert(Schema::OFFSET_NOBLES == 177);
  static_assert(Schema::OFFSET_CURRENT_PLAYER == 195);
  static_assert(TOTAL_FEATURES == Schema::SIZE);

  size_t expected_offset = 0;
  for (const auto &section : Schema::SECTIONS) {
    check(section.offset == expected_offset);
    check(section.size > 0);
    expected_offset += section.size;
  }
  check(expected_offset == Schema::SIZE);
  for (size_t index = 0; index < Schema::GEM_COLOR_IDS.size(); ++index)
    check(Schema::GEM_COLOR_IDS[index] == index);
  check(std::string_view(Schema::fingerprint()) ==
        "csplendor.state.v1;base=196;public=117;gems="
        "diamond,sapphire,emerald,ruby,onyx,gold");

  uint64_t digest = FNV_OFFSET;
  size_t states = 0;
  for (uint32_t seed = 0; seed < 16; ++seed) {
    Game game(seed);
    for (uint32_t ply = 0; ply < 48; ++ply) {
      for (int8_t observer : {int8_t{-1}, int8_t{0}, int8_t{1}}) {
        const auto features = StateEncoder::encode(game, observer);
        for (float feature : features) {
          uint32_t bits = 0;
          static_assert(sizeof(bits) == sizeof(feature));
          std::memcpy(&bits, &feature, sizeof(bits));
          digest_little_endian(digest, bits, 4);
        }
      }
      ++states;
      const auto codes = game.legal_action_codes();
      if (codes.empty())
        break;
      const size_t selected =
          (static_cast<size_t>(seed) * 29 + static_cast<size_t>(ply) * 13) %
          codes.size();
      check(game.apply_action_code_trusted(codes[selected], false));
    }
  }
  check(states == 768);
  check(digest == 0xcde3bf1dd313ae48ULL);
}

} // namespace

int main() {
  test_action_schemas_and_full_id_golden();
  test_state_schema_and_reachable_golden();
  return 0;
}
