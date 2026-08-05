#include "bindings.h"
#include "action.h"
#include "card_data.h"
#include "noble_data.h"
#include "player.h"
#include "types.h"
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace py = pybind11;

namespace csplendor::python {

void bind_domain(py::module_ &m) {
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
      .def_readwrite("purchased_count", &PlayerState::purchased_count)
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
            p.reserved_count = static_cast<uint8_t>(
                std::count_if(p.reserved.begin(), p.reserved.end(),
                              [](int8_t id) { return id != -1; }));
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
      .def("is_token_noop", &Action::is_token_noop,
           "Whether a token-taking action returns exactly the tokens it takes")
      .def("pack", &Action::pack, "Pack action into a compact uint64 code")
      .def_static("unpack", &Action::unpack, py::arg("code"),
                  "Unpack a compact uint64 action code")
      .def("__repr__", &Action::to_string);

  py::enum_<ActionType>(m, "ActionType")
      .value("TAKE_DIFFERENT", TAKE_DIFFERENT)
      .value("TAKE_SAME", TAKE_SAME)
      .value("RESERVE_VISIBLE", RESERVE_VISIBLE)
      .value("RESERVE_DECK", RESERVE_DECK)
      .value("PURCHASE", PURCHASE)
      .value("VISIT_NOBLE", VISIT_NOBLE)
      .value("PASS", PASS)
      .export_values();

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
}

} // namespace csplendor::python
