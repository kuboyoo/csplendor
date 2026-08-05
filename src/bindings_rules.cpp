#include "bindings.h"
#include "board.h"
#include "board_editor.h"
#include "card_data.h"
#include "cli_utils.h"
#include "game.h"
#include "game_snapshot.h"
#include "noble_data.h"
#include "player.h"
#include <pybind11/stl.h>
#include <array>
#include <cstdint>
#include <vector>

namespace py = pybind11;

namespace csplendor::python {

void bind_rules(py::module_ &m) {
  py::class_<Board>(m, "Board")
      .def(py::init<>())
      .def_property(
          "turn", [](const Board &b) { return (int)b.turn; },
          &state::editor::set_turn)
      .def_property(
          "current_player",
          [](const Board &b) { return (int)b.current_player; },
          &state::editor::set_current_player)
      .def_property(
          "bank", [](const Board &b) { return b.bank; },
          &state::editor::set_bank)
      .def_property(
          "visible",
          [](const Board &b) {
            py::list visible(3);
            for (int i = 0; i < 3; ++i) {
              py::list row(Board::CARDS_PER_LEVEL);
              for (int j = 0; j < Board::CARDS_PER_LEVEL; ++j)
                row[j] = static_cast<int>(b.visible[i][j]);
              visible[i] = std::move(row);
            }
            return visible;
          },
          &state::editor::set_visible)
      .def_property(
          "nobles",
          [](const Board &b) {
            py::list nobles(b.nobles.size());
            for (size_t i = 0; i < b.nobles.size(); ++i)
              nobles[i] = static_cast<int>(b.nobles[i]);
            return nobles;
          },
          &state::editor::set_nobles)
      .def_property(
          "decks",
          [](const Board &b) {
            py::list decks(3);
            for (int i = 0; i < 3; ++i) {
              py::list deck(b.decks[i].size());
              for (size_t j = 0; j < b.decks[i].size(); ++j)
                deck[j] = static_cast<int>(b.decks[i][j]);
              decks[i] = std::move(deck);
            }
            return decks;
          },
          &state::editor::set_decks)
      .def_property(
          "final_round", [](const Board &b) { return b.final_round; },
          &state::editor::set_final_round)
      .def_property(
          "waiting_noble", [](const Board &b) { return b.waiting_noble; },
          &state::editor::set_waiting_noble)
      .def_property(
          "winner", [](const Board &b) { return (int)b.winner; },
          &state::editor::set_winner)
      .def_property_readonly("players", [](const Board &b) {
        py::list players(Board::NUM_PLAYERS);
        for (int i = 0; i < Board::NUM_PLAYERS; ++i)
          players[i] = py::cast(b.players[i], py::return_value_policy::copy);
        return players;
      })
      .def("get_player",
           [](const Board &b, int i) {
             if (i < 0 || i >= 2)
               throw py::index_error();
             return b.players[i];
           })
      .def("set_player", &state::editor::set_player)
      .def("hash", &Board::hash)
      .def("observable_hash", &Board::observable_hash, py::arg("observer"),
           "Hash based only on information visible to observer player")
      .def("observable_repetition_hash", &Board::observable_repetition_hash,
           py::arg("observer"),
           "Observable position hash that ignores the monotonic turn counter")
      .def("observable_card_pool", &Board::observable_card_pool,
           py::arg("observer"), py::arg("level"),
           "Return the sorted observer-safe unknown pool for one tier")
      .def("randomize_hidden_information", &Board::randomize_hidden_information,
           py::arg("observer_player"), py::arg("seed"))
      .def("print_board", [](const Board &b) { cli::print_board(b); })
      .def("__repr__", &Board::to_string);

  py::class_<Game>(m, "Game")
      .def(py::init<uint64_t>(), py::arg("seed") = 0)
      .def("clone", &Game::clone)
      .def("clone_light", &Game::clone_light)
      .def(
          "serialize_snapshot",
          [](const Game &game) {
            return py::bytes(csplendor::snapshot::serialize(game));
          },
          "Serialize the current lightweight game state without undo history")
      .def_static(
          "deserialize_snapshot",
          [](py::bytes snapshot) {
            return csplendor::snapshot::deserialize(
                snapshot.cast<std::string>());
          },
          py::arg("snapshot"),
          "Restore a versioned lightweight game-state snapshot")
      .def_static(
          "snapshot_format_version",
          []() {
            return csplendor::snapshot::GAME_SNAPSHOT_FORMAT_VERSION;
          })
      .def_static(
          "snapshot_rules_version",
          []() { return csplendor::snapshot::GAME_SNAPSHOT_RULES_VERSION; })
      .def("shuffled_clone", &Game::shuffled_clone, py::arg("observer_player"),
           py::arg("seed"),
           "Create a clone with hidden information randomized from observer's "
           "perspective")
      .def("board_hash", [](const Game &g) { return g.board.hash(); })
      .def_readonly("board", &Game::board)
      .def("apply", &Game::apply, py::arg("action"),
           py::arg("record_history") = true)
      .def("apply_trusted", &Game::apply_trusted, py::arg("action"),
           py::arg("record_history") = false,
           "Apply an already-known legal action without full validation")
      .def("apply_action_code", &Game::apply_action_code, py::arg("code"),
           py::arg("record_history") = true)
      .def("apply_action_code_trusted", &Game::apply_action_code_trusted,
           py::arg("code"), py::arg("record_history") = false)
      .def("apply_legal_action_index", &Game::apply_legal_action_index,
           py::arg("index"), py::arg("record_history") = false,
           "Generate legal actions internally and apply the selected index")
      .def("apply_random_action", &Game::apply_random_action,
           py::arg("random_value"), py::arg("record_history") = false,
           "Generate legal actions internally and apply random_value % count")
      .def_property_readonly("requires_forced_pass",
                             &Game::requires_forced_pass)
      .def("apply_forced_pass", &Game::apply_forced_pass,
           py::arg("record_history") = true,
           "Apply the forced pass available when no ordinary action exists")
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
      .def_property_readonly("legal_action_count", &Game::legal_action_count)
      .def_property_readonly("legal_action_codes", &Game::legal_action_codes)
      .def("legal_action_code_at", &Game::legal_action_code_at,
           py::arg("index"))
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
}

} // namespace csplendor::python
