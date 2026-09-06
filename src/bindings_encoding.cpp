#include "action.h"
#include "action_encoder.h"
#include "action_encoder_v2.h"
#include "action_encoder_v3.h"
#include "bindings.h"
#include "bindings_array.h"
#include "encoding_schema.h"
#include "game.h"
#include "state_encoder.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <pybind11/stl.h>
#include <string>
#include <utility>
#include <vector>

using csplendor::python::detail::owning_array_copy;

namespace py = pybind11;

namespace csplendor::python {

namespace {

template <typename Schema> py::list action_schema_sections() {
  py::list result;
  for (const auto &section : Schema::SECTIONS) {
    py::dict item;
    item["name"] = section.name;
    item["action_type"] = section.action_type;
    item["offset"] = section.offset;
    item["size"] = section.size;
    result.append(std::move(item));
  }
  return result;
}

py::list state_schema_sections() {
  py::list result;
  for (const auto &section : encoding::StateFeatureV1::SECTIONS) {
    py::dict item;
    item["name"] = section.name;
    item["offset"] = section.offset;
    item["size"] = section.size;
    result.append(std::move(item));
  }
  return result;
}

} // namespace

void bind_encoding(py::module_ &m) {
  py::class_<StateEncoder>(m, "StateEncoder")
      .def_static("schema_version", &StateEncoder::schema_version)
      .def_static("schema_fingerprint", &StateEncoder::schema_fingerprint)
      .def_static("schema_sections", &state_schema_sections)
      .def_static("feature_shape",
                  []() {
                    return std::vector<size_t>(
                        encoding::StateFeatureV1::SHAPE.begin(),
                        encoding::StateFeatureV1::SHAPE.end());
                  })
      .def_static("feature_size",
                  []() { return encoding::StateFeatureV1::SIZE; })
      .def_static("card_feature_size",
                  []() { return encoding::StateFeatureV1::CARD_FEATURE_SIZE; })
      .def_static("noble_feature_size",
                  []() { return encoding::StateFeatureV1::NOBLE_FEATURE_SIZE; })
      .def_static(
          "player_feature_size",
          []() { return encoding::StateFeatureV1::PLAYER_FEATURE_SIZE; })
      .def_static("gem_color_ids",
                  []() {
                    return std::vector<uint8_t>(
                        encoding::StateFeatureV1::GEM_COLOR_IDS.begin(),
                        encoding::StateFeatureV1::GEM_COLOR_IDS.end());
                  })
      .def_static("gem_color_names",
                  []() {
                    return std::vector<std::string>(
                        encoding::StateFeatureV1::GEM_COLOR_NAMES.begin(),
                        encoding::StateFeatureV1::GEM_COLOR_NAMES.end());
                  })
      .def_static(
          "encode",
          [](const Game &game, int8_t observer) {
            auto features = StateEncoder::encode(game, observer);
            return std::vector<float>(features.begin(), features.end());
          },
          py::arg("game"), py::arg("observer") = -1,
          "Encode game state to feature vector")
      .def_static(
          "encode_numpy",
          [](const Game &game, int8_t observer) {
            return owning_array_copy(StateEncoder::encode(game, observer));
          },
          py::arg("game"), py::arg("observer") = -1,
          "Encode to an independent owning C-contiguous float32 array of shape (196,). "
          "Keeps the GIL; never exposes stack or reusable search storage.")
      .def_static(
          "encode_canonical",
          [](const Game &game, int player, int8_t observer) {
            auto features =
                StateEncoder::encode_canonical(game, player, observer);
            return std::vector<float>(features.begin(), features.end());
          },
          py::arg("game"), py::arg("player"), py::arg("observer") = -1,
          "Encode game state with player perspective swap")
      .def_static(
          "encode_public_card_statistics",
          [](const Game &game, int player, uint8_t observer) {
            auto features = StateEncoder::encode_public_card_statistics(
                game, player, observer);
            return std::vector<float>(features.begin(), features.end());
          },
          py::arg("game"), py::arg("player"), py::arg("observer"),
          "Encode observer-safe posterior statistics for future card reveals")
      .def_static("public_card_feature_size",
                  []() { return PUBLIC_CARD_FEATURE_SIZE; });

  // ActionEncoderCpp bindings (native C++ implementation)
  py::class_<ActionEncoderCpp>(m, "ActionEncoderCpp")
      .def_readonly_static("BASE_ACTION_COUNT",
                           &ActionEncoderCpp::BASE_ACTION_COUNT)
      .def_static("schema_version",
                  []() { return ActionEncoderCpp::Schema::VERSION; })
      .def_static("schema_fingerprint",
                  []() { return ActionEncoderCpp::Schema::fingerprint(); })
      .def_static(
          "schema_sections",
          []() { return action_schema_sections<ActionEncoderCpp::Schema>(); })
      .def_static(
          "encode",
          [](const Action &action, const Game &game) {
            return ActionEncoderCpp::encode(action, game);
          },
          py::arg("action"), py::arg("game"),
          "Encode an action to an index [0, 47]")
      .def_static(
          "decode",
          [](int index, const Game &game) {
            return ActionEncoderCpp::decode(index, game);
          },
          py::arg("index"), py::arg("game"),
          "Decode an index [0, 47] to an Action")
      .def_static(
          "get_action_mask",
          [](const Game &game) {
            auto mask = ActionEncoderCpp::get_action_mask(game);
            return owning_array_copy(mask);
          },
          py::arg("game"), "Get a boolean mask of size 48 where 1 means legal")
      .def_static(
          "get_action_mask_with_scores",
          [](const Game &game) {
            auto [mask, scores] =
                ActionEncoderCpp::get_action_mask_with_scores(game);
            return py::make_tuple(owning_array_copy(mask),
                                  owning_array_copy(scores));
          },
          py::arg("game"),
          "Get action mask and heuristic scores (mask, scores)")
      .def_static(
          "get_heuristic_policy",
          [](const Game &game) {
            auto policy = ActionEncoderCpp::get_heuristic_policy(game);
            return owning_array_copy(policy);
          },
          py::arg("game"), "Get normalized heuristic policy distribution");

  // ActionEncoderV2 bindings (full 4869-action space with return + payment
  // patterns)
  py::class_<ActionEncoderV2>(m, "ActionEncoderV2")
      .def_readonly_static("ACTION_SIZE", &ActionEncoderV2::ACTION_SIZE)
      .def_readonly_static("OFFSET_PASS", &ActionEncoderV2::OFFSET_PASS)
      .def_readonly_static("TAKE_DIFF_RETURN_PATTERNS",
                           &ActionEncoderV2::TAKE_DIFF_RETURN_PATTERNS)
      .def_readonly_static("TAKE_SAME_RETURN_PATTERNS",
                           &ActionEncoderV2::TAKE_SAME_RETURN_PATTERNS)
      .def_readonly_static("RESERVE_RETURN_PATTERNS",
                           &ActionEncoderV2::RESERVE_RETURN_PATTERNS)
      .def_readonly_static("PURCHASE_PAYMENT_PATTERNS",
                           &ActionEncoderV2::PURCHASE_PAYMENT_PATTERNS)
      .def_static("schema_version",
                  []() { return ActionEncoderV2::Schema::VERSION; })
      .def_static("schema_fingerprint",
                  []() { return ActionEncoderV2::Schema::fingerprint(); })
      .def_static(
          "schema_sections",
          []() { return action_schema_sections<ActionEncoderV2::Schema>(); })
      .def_static(
          "encode",
          [](const Action &action, const Game &game) {
            return ActionEncoderV2::encode(action, game);
          },
          py::arg("action"), py::arg("game"),
          "Encode an action to action space index [0, 4868]")
      .def_static(
          "decode",
          [](int index, const Game &game) {
            return ActionEncoderV2::decode(index, game);
          },
          py::arg("index"), py::arg("game"),
          "Decode an extended action index to Action template")
      .def_static(
          "decode_and_match",
          [](int index, const Game &game) {
            return ActionEncoderV2::decode_and_match(index, game);
          },
          py::arg("index"), py::arg("game"),
          "Decode action and match to actual legal action with correct details")
      .def_static(
          "get_action_mask",
          [](const Game &game) {
            auto mask = ActionEncoderV2::get_action_mask(game);
            return owning_array_copy(mask);
          },
          py::arg("game"),
          "Get a boolean mask of size 4869 where 1 means legal");

  // ActionEncoderV3 bindings (3133-action space, card ID-based PURCHASE)
  py::class_<ActionEncoderV3>(m, "ActionEncoderV3")
      .def_readonly_static("ACTION_SIZE", &ActionEncoderV3::ACTION_SIZE)
      .def_readonly_static("OFFSET_PURCHASE", &ActionEncoderV3::OFFSET_PURCHASE)
      .def_readonly_static("OFFSET_VISIT_NOBLE",
                           &ActionEncoderV3::OFFSET_VISIT_NOBLE)
      .def_readonly_static("OFFSET_PASS", &ActionEncoderV3::OFFSET_PASS)
      .def_readonly_static("TOTAL_PURCHASE", &ActionEncoderV3::TOTAL_PURCHASE)
      .def_static("schema_version",
                  []() { return ActionEncoderV3::Schema::VERSION; })
      .def_static("schema_fingerprint",
                  []() { return ActionEncoderV3::Schema::fingerprint(); })
      .def_static(
          "schema_sections",
          []() { return action_schema_sections<ActionEncoderV3::Schema>(); })
      .def_static(
          "encode",
          [](const Action &action, const Game &game) {
            return ActionEncoderV3::encode(action, game);
          },
          py::arg("action"), py::arg("game"),
          "Encode an action to action space index [0, 3132]")
      .def_static(
          "decode",
          [](int index, const Game &game) {
            return ActionEncoderV3::decode(index, game);
          },
          py::arg("index"), py::arg("game"),
          "Decode an action index to Action template")
      .def_static(
          "decode_and_match",
          [](int index, const Game &game) {
            return ActionEncoderV3::decode_and_match(index, game);
          },
          py::arg("index"), py::arg("game"),
          "Decode action and match to actual legal action with correct details")
      .def_static(
          "get_action_mask",
          [](const Game &game) {
            auto mask = ActionEncoderV3::get_action_mask(game);
            return owning_array_copy(mask);
          },
          py::arg("game"),
          "Get a boolean mask of size 3133 where 1 means legal")
      .def_static("compute_pattern_count",
                  &ActionEncoderV3::compute_pattern_count, py::arg("card_id"),
                  "Compute the number of valid payment patterns for a card")
      .def_static(
          "encode_payment_for_card",
          [](const std::vector<uint8_t> &gold_as, int card_id) {
            std::array<uint8_t, 5> ga = {0};
            for (size_t i = 0; i < std::min(gold_as.size(), (size_t)5); ++i)
              ga[i] = gold_as[i];
            return ActionEncoderV3::encode_payment_for_card(ga, card_id);
          },
          py::arg("gold_as"), py::arg("card_id"),
          "Encode a gold_as pattern for a specific card")
      .def_static(
          "decode_payment_for_card",
          [](int pattern, int card_id) {
            auto ga =
                ActionEncoderV3::decode_payment_for_card(pattern, card_id);
            return std::vector<uint8_t>(ga.begin(), ga.end());
          },
          py::arg("pattern"), py::arg("card_id"),
          "Decode a payment pattern index for a specific card")
      .def_static(
          "get_card_payment_offset",
          [](int card_id) {
            if (card_id < 0 || card_id >= 90)
              return -1;
            return (int)ActionEncoderV3::CARD_PAYMENT_OFFSET[card_id];
          },
          py::arg("card_id"),
          "Get the payment offset for a card within the PURCHASE range")
      .def_static(
          "get_card_pattern_count",
          [](int card_id) {
            if (card_id < 0 || card_id >= 90)
              return -1;
            return (int)ActionEncoderV3::CARD_PATTERN_COUNT[card_id];
          },
          py::arg("card_id"), "Get the stored pattern count for a card");
}

} // namespace csplendor::python
