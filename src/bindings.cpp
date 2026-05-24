#include "action.h"
#include "action_encoder.h"
#include "action_encoder_v2.h"
#include "action_encoder_v3.h"
#include "board.h"
#include "card_data.h"
#include "cli_utils.h"
#include "game.h"
#include "mcts.h"
#include "mcts_searcher.h"
#include "move_generator.h"
#include "noble_data.h"
#include "player.h"
#include "state_encoder.h"
#include "types.h"
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// Python callback featurizer
class PyFeaturizer : public IFeaturizer {
public:
  PyFeaturizer(py::object featurizer) : featurizer_(featurizer) {}

  std::array<float, 196> featurize(const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = featurizer_.attr("featurize")(py::cast(game));
    py::array_t<float> arr = result.cast<py::array_t<float>>();
    std::array<float, 196> features = {0};
    auto r = arr.unchecked<1>();
    for (ssize_t i = 0; i < std::min(static_cast<ssize_t>(196), r.shape(0));
         ++i) {
      features[i] = r(i);
    }
    return features;
  }

private:
  py::object featurizer_;
};

// Python callback action encoder
class PyActionEncoder : public IActionEncoder {
public:
  PyActionEncoder(py::object encoder) : encoder_(encoder) {}

  int encode(const Action &action, const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result =
        encoder_.attr("encode")(py::cast(action), py::cast(game));
    return result.cast<int>();
  }

  Action decode(int action_idx, const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("decode")(action_idx, py::cast(game));
    if (result.is_none()) {
      return Action(); // Return default action
    }
    return result.cast<Action>();
  }

  std::array<uint8_t, MAX_ACTIONS> get_action_mask(const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("get_action_mask")(py::cast(game));
    py::array_t<uint8_t> arr = result.cast<py::array_t<uint8_t>>();
    std::array<uint8_t, MAX_ACTIONS> mask = {0};
    auto r = arr.unchecked<1>();
    for (ssize_t i = 0;
         i < std::min(static_cast<ssize_t>(MAX_ACTIONS), r.shape(0)); ++i) {
      mask[i] = r(i);
    }
    return mask;
  }

private:
  py::object encoder_;
};

PYBIND11_MODULE(_csplendor, m) {
  py::enum_<GemType>(m, "GemType")
      .value("DIAMOND", DIAMOND)
      .value("SAPPHIRE", SAPPHIRE)
      .value("EMERALD", EMERALD)
      .value("RUBY", RUBY)
      .value("ONYX", ONYX)
      .value("GOLD", GOLD)
      .export_values();

  py::class_<Card>(m, "Card")
      .def_readwrite("id", &Card::id)
      .def_readwrite("level", &Card::level)
      .def_readwrite("points", &Card::points)
      .def_readwrite("bonus", &Card::bonus)
      .def_property_readonly("cost", [](const Card &c) { return c.cost; })
      .def_readwrite("packed_cost", &Card::packed_cost);

  py::class_<Noble>(m, "Noble")
      .def_readwrite("id", &Noble::id)
      .def_readwrite("points", &Noble::points)
      .def_property_readonly("requirement",
                             [](const Noble &n) { return n.requirement; })
      .def_readwrite("packed_requirement", &Noble::packed_requirement);

  py::class_<PlayerState>(m, "PlayerState")
      .def(py::init<>())
      .def_readwrite("points", &PlayerState::points)
      .def_property(
          "gems", [](const PlayerState &p) { return p.gems; },
          [](PlayerState &p, std::array<uint8_t, 6> g) {
            p.gems = g;
            p.sync_packed();
          })
      .def_readwrite("packed_gems", &PlayerState::packed_gems)
      .def_property(
          "bonuses", [](const PlayerState &p) { return p.bonuses; },
          [](PlayerState &p, std::array<uint8_t, 5> b) {
            p.bonuses = b;
            p.sync_packed();
          })
      .def_readwrite("packed_bonuses", &PlayerState::packed_bonuses)
      .def_readwrite("reserved_count", &PlayerState::reserved_count)
      .def_readwrite("reserved_is_hidden", &PlayerState::reserved_is_hidden)
      .def_property(
          "reserved",
          [](const PlayerState &p) {
            std::vector<int8_t> r;
            for (int i = 0; i < 3; ++i)
              r.push_back(p.reserved[i]);
            return r;
          },
          [](PlayerState &p, std::vector<int8_t> r) {
            for (int i = 0; i < 3; ++i) {
              p.reserved[i] = (i < (int)r.size()) ? r[i] : -1;
            }
            p.reserved_count =
                std::count_if(p.reserved.begin(), p.reserved.end(),
                              [](int8_t id) { return id != -1; });
          })
      .def_readwrite("acquired_nobles", &PlayerState::acquired_nobles)
      .def_readwrite("purchased_cards", &PlayerState::purchased_cards);

  py::class_<Action>(m, "Action")
      .def(py::init<>())
      .def_readwrite("type", &Action::type)
      .def_readwrite("take", &Action::take)
      .def_readwrite("card_id", &Action::card_id)
      .def_readwrite("deck_level", &Action::deck_level)
      .def_readwrite("from_reserved", &Action::from_reserved)
      .def_readwrite("gold_as", &Action::gold_as)
      .def_readwrite("return_gems", &Action::return_gems)
      .def_readwrite("noble_choice", &Action::noble_choice)
      .def("__repr__", &Action::to_string);

  py::enum_<ActionType>(m, "ActionType")
      .value("TAKE_DIFFERENT", TAKE_DIFFERENT)
      .value("TAKE_SAME", TAKE_SAME)
      .value("RESERVE_VISIBLE", RESERVE_VISIBLE)
      .value("RESERVE_DECK", RESERVE_DECK)
      .value("PURCHASE", PURCHASE)
      .value("VISIT_NOBLE", VISIT_NOBLE)
      .export_values();

  py::class_<Board>(m, "Board")
      .def(py::init<>())
      .def_property(
          "turn", [](const Board &b) { return (int)b.turn; },
          [](Board &b, int turn) {
            if (turn < 0 || turn > 65535)
              throw py::value_error("turn out of range");
            b.turn = static_cast<uint16_t>(turn);
            b.invalidate_hash();
          })
      .def_property(
          "current_player",
          [](const Board &b) { return (int)b.current_player; },
          [](Board &b, int player) {
            if (player < 0 || player >= Board::NUM_PLAYERS)
              throw py::index_error("current_player out of range");
            b.current_player = static_cast<uint8_t>(player);
            b.invalidate_hash();
          })
      .def_property_readonly("bank", [](const Board &b) { return b.bank; })
      .def_property_readonly("visible",
                             [](const Board &b) {
                               std::vector<std::vector<int8_t>> v(
                                   3, std::vector<int8_t>(4));
                               for (int i = 0; i < 3; ++i)
                                 for (int j = 0; j < 4; ++j)
                                   v[i][j] = b.visible[i][j];
                               return v;
                             })
      .def_property_readonly("nobles",
                             [](const Board &b) {
                               std::vector<uint8_t> v;
                               for (size_t i = 0; i < b.nobles.size(); ++i)
                                 v.push_back(b.nobles[i]);
                               return v;
                             })
      .def_property_readonly("decks",
                             [](const Board &b) {
                               std::vector<std::vector<uint8_t>> d(3);
                               for (int i = 0; i < 3; ++i) {
                                 for (size_t j = 0; j < b.decks[i].size(); ++j)
                                   d[i].push_back(b.decks[i][j]);
                               }
                               return d;
                             })
      .def_property(
          "waiting_noble", [](const Board &b) { return b.waiting_noble; },
          [](Board &b, bool waiting) {
            b.waiting_noble = waiting;
            b.invalidate_hash();
          })
      .def_property(
          "winner", [](const Board &b) { return (int)b.winner; },
          [](Board &b, int winner) {
            if (winner < -2 || winner > 1)
              throw py::value_error("winner out of range");
            b.winner = static_cast<int8_t>(winner);
            b.invalidate_hash();
          })
      .def_property_readonly("players",
                             [](const Board &b) {
                               return std::vector<PlayerState>{b.players[0],
                                                               b.players[1]};
                             })
      .def("get_player",
           [](const Board &b, int i) {
             if (i < 0 || i >= 2)
               throw py::index_error();
             return b.players[i];
           })
      .def("set_player",
           [](Board &b, int i, const PlayerState &p) {
             if (i < 0 || i >= 2)
               throw py::index_error();
             b.players[i] = p;
             b.players[i].sync_packed(); // Crucial for consistency
             b.invalidate_hash();
           })
      .def("hash", &Board::hash)
      .def("observable_hash", &Board::observable_hash, py::arg("observer"),
           "Hash based only on information visible to observer player")
      .def("randomize_hidden_information", &Board::randomize_hidden_information,
           py::arg("observer_player"), py::arg("seed"))
      .def("print_board", [](const Board &b) { cli::print_board(b); })
      .def("__repr__", &Board::to_string);

  py::class_<Game>(m, "Game")
      .def(py::init<uint64_t>(), py::arg("seed") = 0)
      .def("clone", &Game::clone)
      .def("clone_light", &Game::clone_light)
      .def("shuffled_clone", &Game::shuffled_clone, py::arg("observer_player"),
           py::arg("seed"),
           "Create a clone with hidden information randomized from observer's "
           "perspective")
      .def("board_hash", [](const Game &g) { return g.board.hash(); })
      .def_readonly("board", &Game::board)
      .def("apply", &Game::apply, py::arg("action"),
           py::arg("record_history") = true)
      .def("undo", &Game::undo)
      .def("is_legal", &Game::is_legal)
      .def("is_game_over", &Game::is_game_over)
      .def_property_readonly("winner",
                             [](const Game &g) { return g.board.winner; })
      .def_property_readonly(
          "current_player",
          [](const Game &g) { return g.board.current_player; })
      .def_property_readonly("turn",
                             [](const Game &g) { return (int)g.board.turn; })
      .def_property_readonly("scores", &Game::scores)
      .def_property_readonly("legal_actions", &Game::legal_actions)
      .def_property_readonly("base_actions", &Game::base_actions)
      .def_property("simple_payment_mode", &Game::get_simple_payment_mode,
                    &Game::set_simple_payment_mode,
                    "When true, only generate minimal gold payment patterns "
                    "for purchases")
      .def_property("blank_refill_mode", &Game::get_blank_refill_mode,
                    &Game::set_blank_refill_mode,
                    "When true, visible refill from deck is consumed as blank")
      .def("print_board", [](const Game &g) { cli::print_board(g.board); })
      .def("print_legal_actions", [](const Game &g) {
        cli::print_legal_actions(g.board, g.legal_actions());
      });

  m.def("get_card", &get_card, py::arg("id"));
  m.def("get_noble", &get_noble, py::arg("id"));
  m.def("get_all_cards", []() {
    std::vector<Card> cards;
    for (int i = 0; i < CARD_COUNT; ++i)
      cards.push_back(CARDS[i]);
    return cards;
  });
  m.def("get_all_nobles", []() {
    std::vector<Noble> nobles;
    for (int i = 0; i < NOBLE_COUNT; ++i)
      nobles.push_back(NOBLES[i]);
    return nobles;
  });

  // StateEncoder bindings
  py::class_<StateEncoder>(m, "StateEncoder")
      .def_static(
          "encode",
          [](const Game &game, int8_t observer) {
            auto features = StateEncoder::encode(game, observer);
            return std::vector<float>(features.begin(), features.end());
          },
          py::arg("game"), py::arg("observer") = -1,
          "Encode game state to feature vector")
      .def_static(
          "encode_canonical",
          [](const Game &game, int player, int8_t observer) {
            auto features =
                StateEncoder::encode_canonical(game, player, observer);
            return std::vector<float>(features.begin(), features.end());
          },
          py::arg("game"), py::arg("player"), py::arg("observer") = -1,
          "Encode game state with player perspective swap");

  // ActionEncoderCpp bindings (native C++ implementation)
  py::class_<ActionEncoderCpp>(m, "ActionEncoderCpp")
      .def_readonly_static("BASE_ACTION_COUNT",
                           &ActionEncoderCpp::BASE_ACTION_COUNT)
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
            return py::array_t<uint8_t>({ActionEncoderCpp::BASE_ACTION_COUNT},
                                        mask.data());
          },
          py::arg("game"), "Get a boolean mask of size 48 where 1 means legal")
      .def_static(
          "get_action_mask_with_scores",
          [](const Game &game) {
            auto [mask, scores] =
                ActionEncoderCpp::get_action_mask_with_scores(game);
            return py::make_tuple(
                py::array_t<uint8_t>({ActionEncoderCpp::BASE_ACTION_COUNT},
                                     mask.data()),
                py::array_t<float>({ActionEncoderCpp::BASE_ACTION_COUNT},
                                   scores.data()));
          },
          py::arg("game"),
          "Get action mask and heuristic scores (mask, scores)")
      .def_static(
          "get_heuristic_policy",
          [](const Game &game) {
            auto policy = ActionEncoderCpp::get_heuristic_policy(game);
            return py::array_t<float>({ActionEncoderCpp::BASE_ACTION_COUNT},
                                      policy.data());
          },
          py::arg("game"), "Get normalized heuristic policy distribution");

  // ActionEncoderV2 bindings (full 4869-action space with return + payment patterns)
  py::class_<ActionEncoderV2>(m, "ActionEncoderV2")
      .def_readonly_static("ACTION_SIZE", &ActionEncoderV2::ACTION_SIZE)
      .def_readonly_static("TAKE_DIFF_RETURN_PATTERNS",
                           &ActionEncoderV2::TAKE_DIFF_RETURN_PATTERNS)
      .def_readonly_static("TAKE_SAME_RETURN_PATTERNS",
                           &ActionEncoderV2::TAKE_SAME_RETURN_PATTERNS)
      .def_readonly_static("RESERVE_RETURN_PATTERNS",
                           &ActionEncoderV2::RESERVE_RETURN_PATTERNS)
      .def_readonly_static("PURCHASE_PAYMENT_PATTERNS",
                           &ActionEncoderV2::PURCHASE_PAYMENT_PATTERNS)
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
            return py::array_t<uint8_t>({ActionEncoderV2::ACTION_SIZE},
                                        mask.data());
          },
          py::arg("game"),
          "Get a boolean mask of size 4869 where 1 means legal");

  // ActionEncoderV3 bindings (3124-action space, card ID-based PURCHASE)
  py::class_<ActionEncoderV3>(m, "ActionEncoderV3")
      .def_readonly_static("ACTION_SIZE", &ActionEncoderV3::ACTION_SIZE)
      .def_readonly_static("OFFSET_PURCHASE", &ActionEncoderV3::OFFSET_PURCHASE)
      .def_readonly_static("OFFSET_VISIT_NOBLE",
                           &ActionEncoderV3::OFFSET_VISIT_NOBLE)
      .def_readonly_static("OFFSET_PASS", &ActionEncoderV3::OFFSET_PASS)
      .def_readonly_static("TOTAL_PURCHASE", &ActionEncoderV3::TOTAL_PURCHASE)
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
            return py::array_t<uint8_t>({ActionEncoderV3::ACTION_SIZE},
                                        mask.data());
          },
          py::arg("game"),
          "Get a boolean mask of size 3133 where 1 means legal")
      .def_static(
          "compute_pattern_count", &ActionEncoderV3::compute_pattern_count,
          py::arg("card_id"),
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
            auto ga = ActionEncoderV3::decode_payment_for_card(pattern, card_id);
            return std::vector<uint8_t>(ga.begin(), ga.end());
          },
          py::arg("pattern"), py::arg("card_id"),
          "Decode a payment pattern index for a specific card")
      .def_static(
          "get_card_payment_offset",
          [](int card_id) {
            if (card_id < 0 || card_id >= 90) return -1;
            return (int)ActionEncoderV3::CARD_PAYMENT_OFFSET[card_id];
          },
          py::arg("card_id"),
          "Get the payment offset for a card within the PURCHASE range")
      .def_static(
          "get_card_pattern_count",
          [](int card_id) {
            if (card_id < 0 || card_id >= 90) return -1;
            return (int)ActionEncoderV3::CARD_PATTERN_COUNT[card_id];
          },
          py::arg("card_id"),
          "Get the stored pattern count for a card");

  // MCTS bindings
  py::class_<MCTSConfig>(m, "MCTSConfig")
      .def(py::init<>())
      .def_readwrite("cpuct", &MCTSConfig::cpuct)
      .def_readwrite("dirichlet_alpha", &MCTSConfig::dirichlet_alpha)
      .def_readwrite("dirichlet_epsilon", &MCTSConfig::dirichlet_epsilon)
      .def_readwrite("use_dirichlet_noise", &MCTSConfig::use_dirichlet_noise)
      .def_readwrite("use_determinization", &MCTSConfig::use_determinization)
      .def_readwrite("num_simulations", &MCTSConfig::num_simulations)
      .def_readwrite("num_determinizations", &MCTSConfig::num_determinizations)
      .def_readwrite("fpu", &MCTSConfig::fpu)
      .def_readwrite("forced_playouts", &MCTSConfig::forced_playouts)
      .def_readwrite("forced_playouts_k", &MCTSConfig::forced_playouts_k);

  py::class_<MCTSNode>(m, "MCTSNode")
      .def(py::init<>())
      .def_readonly("total_visits", &MCTSNode::total_visits)
      .def_readonly("is_terminal", &MCTSNode::is_terminal)
      .def_readonly("is_expanded", &MCTSNode::is_expanded)
      .def_property_readonly("valid_actions",
                             [](const MCTSNode &n) {
                               std::vector<uint8_t> v(n.valid_actions.begin(),
                                                      n.valid_actions.end());
                               return v;
                             })
      .def_property_readonly("prior",
                             [](const MCTSNode &n) {
                               std::vector<float> v(n.prior.begin(),
                                                    n.prior.end());
                               return v;
                             })
      .def_property_readonly("Q",
                             [](const MCTSNode &n) {
                               std::vector<float> v(n.Q.begin(), n.Q.end());
                               return v;
                             })
      .def_property_readonly("N",
                             [](const MCTSNode &n) {
                               std::vector<uint32_t> v(n.N.begin(), n.N.end());
                               return v;
                             })
      .def_property_readonly("virtual_loss",
                             [](const MCTSNode &n) {
                               std::vector<int32_t> v(n.virtual_loss.begin(),
                                                      n.virtual_loss.end());
                               return v;
                             })
      .def_property_readonly("value", [](const MCTSNode &n) {
        std::vector<float> v(n.value.begin(), n.value.end());
        return v;
      });

  py::class_<MCTS>(m, "MCTS")
      .def(py::init<const MCTSConfig &>())
      .def("clear", &MCTS::clear)
      .def("tree_size", &MCTS::tree_size)
      .def("prune_if_needed", &MCTS::prune_if_needed)
      .def("get_node",
           [](MCTS &mcts, uint64_t hash) -> py::object {
             MCTSNode *node = mcts.get_node(hash);
             if (node)
               return py::cast(*node);
             return py::none();
           })
      .def("expand_node",
           [](MCTS &mcts, uint64_t hash, const std::vector<float> &policy,
              const std::vector<float> &value,
              const std::vector<uint8_t> &valid_actions) {
             std::array<float, MAX_ACTIONS> policy_arr = {0};
             std::array<float, NUM_PLAYERS> value_arr = {0};
             std::array<uint8_t, MAX_ACTIONS> valid_arr = {0};

             for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
               policy_arr[i] = policy[i];
             for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
               value_arr[i] = value[i];
             for (size_t i = 0; i < valid_actions.size() && i < MAX_ACTIONS;
                  ++i)
               valid_arr[i] = valid_actions[i];

             mcts.expand_node(hash, policy_arr, value_arr, valid_arr);
           })
      .def("get_action_probs",
           [](const MCTS &mcts, uint64_t hash, float temperature) {
             auto probs = mcts.get_action_probs(hash, temperature);
             return std::vector<float>(probs.begin(), probs.end());
           })
      .def("update_stats", &MCTS::update_stats, py::arg("hash"),
           py::arg("action"), py::arg("value"),
           "Update node statistics after backpropagation")
      .def(
          "select_action_with_virtual_loss",
          [](MCTS &mcts, uint64_t hash, bool is_root,
             py::object dirichlet_noise_obj, int current_sim) {
            if (dirichlet_noise_obj.is_none()) {
              return mcts.select_action_with_virtual_loss(hash, is_root,
                                                          nullptr, current_sim);
            }
            std::vector<float> noise_vec =
                dirichlet_noise_obj.cast<std::vector<float>>();
            std::array<float, MAX_ACTIONS> noise = {0};
            for (size_t i = 0; i < noise_vec.size() && i < MAX_ACTIONS; ++i) {
              noise[i] = noise_vec[i];
            }
            return mcts.select_action_with_virtual_loss(hash, is_root, &noise,
                                                        current_sim);
          },
          py::arg("hash"), py::arg("is_root"),
          py::arg("dirichlet_noise") = py::none(), py::arg("current_sim") = 0,
          "Select action with virtual loss for parallel MCTS (supports FPU and "
          "forced playouts)")
      .def("add_virtual_loss", &MCTS::add_virtual_loss, py::arg("hash"),
           py::arg("action"), "Add virtual loss to an action")
      .def("remove_virtual_loss", &MCTS::remove_virtual_loss, py::arg("hash"),
           py::arg("action"), "Remove virtual loss from an action")
      .def("clear_virtual_losses", &MCTS::clear_virtual_losses,
           "Clear all virtual losses")
      .def(
          "generate_dirichlet_noise",
          [](MCTS &mcts, uint64_t hash) {
            auto noise = mcts.generate_dirichlet_noise_for_node(hash);
            return std::vector<float>(noise.begin(), noise.end());
          },
          py::arg("hash"), "Generate Dirichlet noise for a node")
      .def(
          "prepare_batch_simulations",
          [](MCTS &mcts, const Game &root_game, uint8_t observer,
             int batch_size, int num_determinizations,
             py::object dirichlet_noise_obj) {
            // Convert dirichlet noise
            const std::array<float, MAX_ACTIONS> *noise_ptr = nullptr;
            std::array<float, MAX_ACTIONS> noise = {0};
            if (!dirichlet_noise_obj.is_none()) {
              std::vector<float> noise_vec =
                  dirichlet_noise_obj.cast<std::vector<float>>();
              for (size_t i = 0; i < noise_vec.size() && i < MAX_ACTIONS; ++i) {
                noise[i] = noise_vec[i];
              }
              noise_ptr = &noise;
            }

            // Use native C++ ActionEncoder (no GIL contention!)
            auto result =
                mcts.prepare_batch_simulations(root_game, observer, batch_size,
                                               num_determinizations, noise_ptr);

            // Convert to Python-friendly format
            py::dict py_result;

            // Flatten encoded boards and valid actions for batch NN inference
            py::list flat_boards;
            py::list flat_valids;
            py::list leaf_world_counts;
            py::list leaf_hashes;
            py::list leaf_paths;

            for (const auto &leaf : result.leaves) {
              leaf_hashes.append(leaf.hash);
              leaf_world_counts.append(leaf.num_worlds);

              // Convert path to Python list
              py::list py_path;
              for (const auto &entry : leaf.path) {
                py::tuple t =
                    py::make_tuple(entry.hash, entry.action, entry.player);
                py_path.append(t);
              }
              leaf_paths.append(py_path);

              // Add boards and valid actions
              for (const auto &board : leaf.encoded_boards) {
                flat_boards.append(
                    py::array_t<float>({FEATURE_SIZE}, board.data()));
              }
              for (const auto &valid : leaf.valid_actions) {
                flat_valids.append(
                    py::array_t<uint8_t>({MAX_ACTIONS}, valid.data()));
              }
            }

            // Handle terminals
            py::list py_terminals;
            for (const auto &[path, value] : result.terminals) {
              py::list py_path;
              for (const auto &entry : path) {
                py::tuple t =
                    py::make_tuple(entry.hash, entry.action, entry.player);
                py_path.append(t);
              }
              py::list py_value;
              for (auto v : value)
                py_value.append(v);
              py_terminals.append(py::make_tuple(py_path, py_value));
            }

            py_result["flat_boards"] = flat_boards;
            py_result["flat_valids"] = flat_valids;
            py_result["leaf_world_counts"] = leaf_world_counts;
            py_result["leaf_hashes"] = leaf_hashes;
            py_result["leaf_paths"] = leaf_paths;
            py_result["terminals"] = py_terminals;
            py_result["total_boards"] = result.total_boards;
            py_result["num_leaves"] = static_cast<int>(result.leaves.size());

            return py_result;
          },
          py::arg("root_game"), py::arg("observer"), py::arg("batch_size"),
          py::arg("num_determinizations"), py::arg("dirichlet_noise"),
          "Prepare batch simulations for NN evaluation (uses native C++ "
          "ActionEncoder)")
      .def(
          "apply_batch_results",
          [](MCTS &mcts, py::dict request, py::list policies, py::list values) {
            // Reconstruct leaves from request
            py::list leaf_hashes = request["leaf_hashes"].cast<py::list>();
            py::list leaf_world_counts =
                request["leaf_world_counts"].cast<py::list>();
            py::list leaf_paths = request["leaf_paths"].cast<py::list>();
            py::list terminals = request["terminals"].cast<py::list>();

            size_t result_idx = 0;

            // Process each leaf
            for (size_t i = 0; i < py::len(leaf_hashes); ++i) {
              uint64_t hash = leaf_hashes[i].cast<uint64_t>();
              int num_worlds = leaf_world_counts[i].cast<int>();
              py::list py_path = leaf_paths[i].cast<py::list>();

              // Reconstruct path
              std::vector<PathEntry> path;
              for (auto item : py_path) {
                py::tuple t = item.cast<py::tuple>();
                PathEntry entry;
                entry.hash = t[0].cast<uint64_t>();
                entry.action = t[1].cast<int>();
                entry.player = t[2].cast<int>();
                path.push_back(entry);
              }

              // Average policy and value across worlds
              std::array<float, MAX_ACTIONS> avg_policy = {0};
              std::array<float, NUM_PLAYERS> avg_value = {0};
              std::array<uint8_t, MAX_ACTIONS> combined_valid = {0};

              for (int w = 0; w < num_worlds; ++w) {
                py::array_t<float> policy =
                    policies[result_idx].cast<py::array_t<float>>();
                py::array_t<float> value =
                    values[result_idx].cast<py::array_t<float>>();
                auto p = policy.unchecked<1>();
                auto v = value.unchecked<1>();

                for (ssize_t a = 0;
                     a <
                     std::min(p.shape(0), static_cast<ssize_t>(MAX_ACTIONS));
                     ++a) {
                  avg_policy[a] += p(a);
                  if (p(a) > 0)
                    combined_valid[a] = 1;
                }
                for (ssize_t j = 0;
                     j <
                     std::min(v.shape(0), static_cast<ssize_t>(NUM_PLAYERS));
                     ++j) {
                  avg_value[j] += v(j);
                }
                result_idx++;
              }

              // Normalize
              float world_count = static_cast<float>(num_worlds);
              for (size_t a = 0; a < MAX_ACTIONS; ++a) {
                avg_policy[a] /= world_count;
              }
              for (size_t j = 0; j < NUM_PLAYERS; ++j) {
                avg_value[j] /= world_count;
              }

              // Re-normalize policy
              float policy_sum = 0.0f;
              for (size_t a = 0; a < MAX_ACTIONS; ++a) {
                if (combined_valid[a]) {
                  policy_sum += avg_policy[a];
                } else {
                  avg_policy[a] = 0.0f;
                }
              }
              if (policy_sum > EPS) {
                for (size_t a = 0; a < MAX_ACTIONS; ++a) {
                  avg_policy[a] /= policy_sum;
                }
              }

              // Expand node
              mcts.expand_node(hash, avg_policy, avg_value, combined_valid);

              // Backpropagate
              mcts.backpropagate_with_virtual_loss_removal(path, avg_value);
            }

            // Handle terminals
            for (auto item : terminals) {
              py::tuple t = item.cast<py::tuple>();
              py::list py_path = t[0].cast<py::list>();
              py::list py_value = t[1].cast<py::list>();

              std::vector<PathEntry> path;
              for (auto p_item : py_path) {
                py::tuple pt = p_item.cast<py::tuple>();
                PathEntry entry;
                entry.hash = pt[0].cast<uint64_t>();
                entry.action = pt[1].cast<int>();
                entry.player = pt[2].cast<int>();
                path.push_back(entry);
              }

              std::array<float, NUM_PLAYERS> value = {0};
              for (size_t j = 0; j < py::len(py_value) && j < NUM_PLAYERS;
                   ++j) {
                value[j] = py_value[j].cast<float>();
              }

              mcts.backpropagate_with_virtual_loss_removal(path, value);
            }
          },
          py::arg("request"), py::arg("policies"), py::arg("values"),
          "Apply batch NN results to the tree")
      .def_property_readonly(
          "config", [](MCTS &mcts) -> MCTSConfig & { return mcts.config(); },
          py::return_value_policy::reference_internal);

  // LeafRequest binding
  py::class_<LeafRequest>(m, "LeafRequest")
      .def(py::init<>())
      .def_readonly("hash", &LeafRequest::hash)
      .def_property_readonly("features",
                             [](const LeafRequest &req) {
                               return std::vector<float>(req.features.begin(),
                                                         req.features.end());
                             })
      .def_property_readonly("valid_actions",
                             [](const LeafRequest &req) {
                               return std::vector<uint8_t>(
                                   req.valid_actions.begin(),
                                   req.valid_actions.end());
                             })
      .def_readonly("path_index", &LeafRequest::path_index);

  // InferenceResult binding
  py::class_<InferenceResult>(m, "InferenceResult")
      .def(py::init<>())
      .def(py::init([](const std::vector<float> &policy,
                       const std::vector<float> &value) {
             InferenceResult res;
             for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
               res.policy[i] = policy[i];
             for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
               res.value[i] = value[i];
             return res;
           }),
           py::arg("policy"), py::arg("value"))
      .def_property(
          "policy",
          [](const InferenceResult &res) {
            return std::vector<float>(res.policy.begin(), res.policy.end());
          },
          [](InferenceResult &res, const std::vector<float> &policy) {
            for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
              res.policy[i] = policy[i];
          })
      .def_property(
          "value",
          [](const InferenceResult &res) {
            return std::vector<float>(res.value.begin(), res.value.end());
          },
          [](InferenceResult &res, const std::vector<float> &value) {
            for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
              res.value[i] = value[i];
          });

  // MCTSSearcher binding with Python callbacks
  m.def(
      "create_mcts_searcher",
      [](MCTS &mcts, py::object featurizer, py::object encoder) {
        auto py_feat = std::make_shared<PyFeaturizer>(featurizer);
        auto py_enc = std::make_shared<PyActionEncoder>(encoder);
        return std::make_tuple(
            std::make_shared<MCTSSearcher>(mcts, *py_feat, *py_enc), py_feat,
            py_enc);
      },
      py::arg("mcts"), py::arg("featurizer"), py::arg("encoder"),
      "Create an MCTSSearcher with Python featurizer and encoder");

  // Full search function that runs entirely in C++ with Python inference
  // callback
  m.def(
      "mcts_search",
      [](MCTS &mcts, py::object featurizer, py::object encoder,
         const Game &root_game, int num_simulations, py::function inference_fn,
         float temperature) {
        PyFeaturizer py_feat(featurizer);
        PyActionEncoder py_enc(encoder);
        MCTSSearcher searcher(mcts, py_feat, py_enc);

        // Inference callback wrapper
        auto cpp_inference =
            [&inference_fn](const std::vector<LeafRequest> &requests)
            -> std::vector<InferenceResult> {
          py::gil_scoped_acquire acquire;

          // Convert LeafRequests to Python-friendly format
          py::list py_requests;
          for (const auto &req : requests) {
            py::dict d;
            d["hash"] = req.hash;
            d["features"] =
                py::array_t<float>({196}, {sizeof(float)}, req.features.data());
            d["valid_actions"] = py::array_t<uint8_t>(
                {MAX_ACTIONS}, {sizeof(uint8_t)}, req.valid_actions.data());
            d["path_index"] = req.path_index;
            py_requests.append(d);
          }

          // Call Python inference function
          py::object result = inference_fn(py_requests);
          py::list results = result.cast<py::list>();

          std::vector<InferenceResult> cpp_results;
          for (auto item : results) {
            InferenceResult ir;
            py::dict d = item.cast<py::dict>();
            py::array_t<float> policy = d["policy"].cast<py::array_t<float>>();
            py::array_t<float> value = d["value"].cast<py::array_t<float>>();

            auto p = policy.unchecked<1>();
            for (ssize_t i = 0;
                 i < std::min(static_cast<ssize_t>(MAX_ACTIONS), p.shape(0));
                 ++i)
              ir.policy[i] = p(i);

            auto v = value.unchecked<1>();
            for (ssize_t i = 0;
                 i < std::min(static_cast<ssize_t>(NUM_PLAYERS), v.shape(0));
                 ++i)
              ir.value[i] = v(i);

            cpp_results.push_back(ir);
          }
          return cpp_results;
        };

        // Run search
        {
          py::gil_scoped_release release;
          searcher.search(root_game, num_simulations, cpp_inference);
        }

        // Get action probabilities
        auto probs = searcher.get_action_probs(root_game, temperature);
        return std::vector<float>(probs.begin(), probs.end());
      },
      py::arg("mcts"), py::arg("featurizer"), py::arg("encoder"),
      py::arg("root_game"), py::arg("num_simulations"), py::arg("inference_fn"),
      py::arg("temperature"),
      "Run full MCTS search with C++ searcher and Python inference callback");
}
