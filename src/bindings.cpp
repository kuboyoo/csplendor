#include "action.h"
#include "action_encoder.h"
#include "action_encoder_v2.h"
#include "action_encoder_v3.h"
#include "board.h"
#include "card_data.h"
#include "cli_utils.h"
#include "game.h"
#include "game_snapshot.h"
#include "mcts.h"
#include "mcts_parallel_searcher.h"
#include "mcts_root_parallel.h"
#include "mcts_searcher.h"
#include "move_generator.h"
#include "noble_data.h"
#include "player.h"
#include "reveal_verified_solver.h"
#include "state_encoder.h"
#include "types.h"
#include "visible_only_solver.h"
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <unordered_map>

namespace py = pybind11;

namespace {

template <typename T, size_t N>
py::array_t<T> owning_array_copy(const std::array<T, N> &source) {
  py::array_t<T> result(N);
  std::memcpy(result.mutable_data(), source.data(), N * sizeof(T));
  return result;
}

mcts_parallel::ParallelInferenceFunction
make_python_parallel_inference(py::function &inference_fn) {
  return [&inference_fn](
             const std::vector<mcts_parallel::ParallelInferenceRequest>
                 &requests) {
    // Root-parallel serialization is owned by its native runner. It locks
    // before acquiring the GIL and can therefore re-check timeout/cancel after
    // waiting instead of draining stale Python callback work.
    py::gil_scoped_acquire acquire;
    py::list python_requests(requests.size());
    for (size_t index = 0; index < requests.size(); ++index) {
      const auto &request = requests[index];
      py::dict key;
      key["position_hash"] = request.key.position_hash;
      key["key_version"] = request.key.key_version;
      key["observer"] = request.key.observer;
      key["domain"] = static_cast<uint8_t>(request.key.domain);
      key["mode_bits"] = request.key.mode_bits;
      py::dict item;
      item["pending_id"] = request.pending_id;
      item["simulation_id"] = request.owner_simulation_id;
      item["tree_key"] = std::move(key);
      // Callbacks may retain these arrays after returning. Every request owns
      // independent contiguous storage.
      item["features"] = owning_array_copy(request.features);
      item["valid_actions"] = owning_array_copy(request.owner_world_mask);
      python_requests[index] = std::move(item);
    }

    py::object callback_result = inference_fn(python_requests);
    py::sequence sequence = callback_result.cast<py::sequence>();
    if (py::len(sequence) != requests.size())
      throw py::value_error(
          "parallel inference result count does not match requests");
    std::vector<mcts_parallel::ParallelInferenceResult> results;
    results.reserve(requests.size());
    for (size_t index = 0; index < requests.size(); ++index) {
      py::dict item = sequence[index].cast<py::dict>();
      if (!item.contains("policy") || !item.contains("value"))
        throw py::value_error(
            "parallel inference result requires policy and value");
      py::array_t<float, py::array::c_style | py::array::forcecast> policy =
          item["policy"].cast<py::array_t<
              float, py::array::c_style | py::array::forcecast>>();
      py::array_t<float, py::array::c_style | py::array::forcecast> value =
          item["value"].cast<py::array_t<
              float, py::array::c_style | py::array::forcecast>>();
      if (policy.ndim() != 1 || value.ndim() != 1 ||
          policy.shape(0) < static_cast<ssize_t>(MAX_ACTIONS) ||
          value.shape(0) < static_cast<ssize_t>(NUM_PLAYERS))
        throw py::value_error(
            "parallel policy/value arrays have invalid shape");
      mcts_parallel::ParallelInferenceResult result;
      std::copy_n(policy.data(), MAX_ACTIONS, result.policy.begin());
      std::copy_n(value.data(), NUM_PLAYERS, result.value.begin());
      results.push_back(result);
    }
    return results;
  };
}

constexpr size_t kProofDagCardMaskBytes = (CARD_COUNT + 7) / 8;

std::string proof_dag_card_mask_hex(const std::vector<int> &cards) {
  std::array<uint8_t, kProofDagCardMaskBytes> mask = {0};
  for (int card : cards) {
    if (card < 0 || card >= CARD_COUNT)
      continue;
    mask[static_cast<size_t>(card) / 8] |=
        static_cast<uint8_t>(1u << (static_cast<size_t>(card) % 8));
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t byte : mask)
    out << std::setw(2) << static_cast<int>(byte);
  return out.str();
}

std::string proof_dag_action_key(const RevealVerifiedProofEdge &edge) {
  std::ostringstream out;
  out << edge.action_code << "|" << edge.oracle_card << "|"
      << edge.oracle_reserve << "|" << edge.oracle_reserve_card << "|"
      << edge.oracle_return_color;
  for (uint8_t value : edge.oracle_gold_as)
    out << "|" << static_cast<int>(value);
  return out.str();
}

size_t intern_proof_dag_string(std::map<std::string, size_t> &ids,
                               py::list &values,
                               const std::string &value) {
  auto known = ids.find(value);
  if (known != ids.end())
    return known->second;
  const size_t index = ids.size();
  ids.emplace(value, index);
  values.append(value);
  return index;
}

size_t intern_proof_dag_action(
    std::unordered_map<std::string, size_t> &ids, py::list &actions,
    const RevealVerifiedProofEdge &edge) {
  const std::string key = proof_dag_action_key(edge);
  auto known = ids.find(key);
  if (known != ids.end())
    return known->second;
  const size_t index = ids.size();
  ids.emplace(key, index);

  py::list gold_as;
  for (uint8_t value : edge.oracle_gold_as)
    gold_as.append(static_cast<int>(value));

  py::list item;
  item.append(edge.action_code);
  item.append(edge.oracle_card);
  item.append(edge.oracle_reserve ? 1 : 0);
  item.append(edge.oracle_reserve_card);
  item.append(edge.oracle_return_color);
  item.append(gold_as);
  actions.append(item);
  return index;
}

size_t intern_proof_dag_reveal_group(
    std::unordered_map<std::string, size_t> &ids, py::list &groups,
    std::vector<int> cards) {
  std::sort(cards.begin(), cards.end());
  cards.erase(std::unique(cards.begin(), cards.end()), cards.end());
  const std::string key = proof_dag_card_mask_hex(cards);
  auto known = ids.find(key);
  if (known != ids.end())
    return known->second;
  const size_t index = ids.size();
  ids.emplace(key, index);
  groups.append(key);
  return index;
}

py::dict proof_dag_to_py_v1(const RevealVerifiedProofDag &dag) {
  py::dict proof_dag;
  proof_dag["requested"] = dag.requested;
  proof_dag["complete"] = dag.complete;
  proof_dag["validated"] = dag.validated;
  proof_dag["omitted_reason"] =
      dag.omitted_reason.empty() ? py::none() : py::cast(dag.omitted_reason);
  proof_dag["root"] = dag.complete ? py::cast(dag.root) : py::none();
  py::list proof_nodes;
  for (const RevealVerifiedProofNode &node : dag.nodes) {
    py::dict item;
    item["id"] = node.id;
    item["player"] = node.player;
    item["depth"] = node.depth;
    item["kind"] = node.kind;
    item["scores"] = node.scores;
    item["winner"] = node.winner;
    item["waiting_noble"] = node.waiting_noble;
    item["nobles"] = node.nobles;
    item["acquired_nobles"] = node.acquired_nobles;
    item["resolution"] =
        node.resolution.empty() ? py::none() : py::cast(node.resolution);
    py::list children;
    for (const RevealVerifiedProofEdge &edge : node.children) {
      py::dict child;
      child["action_code"] = edge.action_code;
      child["reveal_card"] =
          edge.reveal_card < 0 ? py::none() : py::cast(edge.reveal_card);
      child["oracle_card"] =
          edge.oracle_card < 0 ? py::none() : py::cast(edge.oracle_card);
      child["oracle_reserve"] = edge.oracle_reserve;
      child["oracle_reserve_card"] =
          edge.oracle_reserve_card < 0 ? py::none()
                                       : py::cast(edge.oracle_reserve_card);
      child["oracle_return_color"] =
          edge.oracle_return_color < 0 ? py::none()
                                       : py::cast(edge.oracle_return_color);
      child["oracle_gold_as"] = edge.oracle_gold_as;
      child["child"] = edge.child;
      children.append(child);
    }
    item["children"] = children;
    proof_nodes.append(item);
  }
  proof_dag["nodes"] = proof_nodes;
  return proof_dag;
}

py::dict proof_dag_to_py_compact(const RevealVerifiedProofDag &dag) {
  py::dict compact;
  compact["format"] = "strategy_dag_compact_v1";
  compact["requested"] = dag.requested;
  compact["complete"] = dag.complete;
  compact["validated"] = dag.validated;
  compact["omitted_reason"] =
      dag.omitted_reason.empty() ? py::none() : py::cast(dag.omitted_reason);
  compact["root"] = dag.complete ? py::cast(dag.root) : py::none();
  compact["reveal_group_encoding"] = "card_bitset_le_hex_v1";

  compact["node_columns"] =
      py::make_tuple("id", "player", "depth", "kind", "resolution",
                     "edge_start", "edge_count");
  compact["edge_columns"] = py::make_tuple("action", "reveal_group", "child");
  compact["action_template_columns"] =
      py::make_tuple("action_code", "oracle_card", "oracle_reserve",
                     "oracle_reserve_card", "oracle_return_color",
                     "oracle_gold_as");

  py::list kind_strings;
  py::list resolution_strings;
  py::list action_templates;
  py::list reveal_groups;
  py::list nodes;
  py::list edges;
  std::map<std::string, size_t> kind_ids;
  std::map<std::string, size_t> resolution_ids;
  std::unordered_map<std::string, size_t> action_ids;
  std::unordered_map<std::string, size_t> reveal_group_ids;

  size_t edge_count = 0;
  for (const RevealVerifiedProofNode &node : dag.nodes) {
    const size_t edge_start = edge_count;
    struct LocalGroup {
      size_t action = 0;
      int reveal_group = -1;
      size_t child = 0;
      bool has_reveal = false;
      std::vector<int> reveal_cards;
    };

    std::vector<LocalGroup> groups;
    std::unordered_map<std::string, size_t> group_ids;
    for (const RevealVerifiedProofEdge &edge : node.children) {
      const size_t action_index =
          intern_proof_dag_action(action_ids, action_templates, edge);
      const bool has_reveal = edge.reveal_card >= 0;
      std::ostringstream key;
      key << action_index << "|" << edge.child << "|" << has_reveal;
      const std::string key_text = key.str();
      auto known = group_ids.find(key_text);
      if (known == group_ids.end()) {
        known = group_ids.emplace(key_text, groups.size()).first;
        groups.push_back(LocalGroup{action_index, -1, edge.child, has_reveal,
                                    {}});
      }
      if (has_reveal)
        groups[known->second].reveal_cards.push_back(edge.reveal_card);
    }

    for (LocalGroup &group : groups) {
      int reveal_group = -1;
      if (group.has_reveal) {
        reveal_group = static_cast<int>(intern_proof_dag_reveal_group(
            reveal_group_ids, reveal_groups, group.reveal_cards));
      }
      py::list item;
      item.append(group.action);
      item.append(reveal_group);
      item.append(group.child);
      edges.append(item);
      ++edge_count;
    }

    const int kind_index = static_cast<int>(
        intern_proof_dag_string(kind_ids, kind_strings, node.kind));
    const int resolution_index =
        node.resolution.empty()
            ? -1
            : static_cast<int>(intern_proof_dag_string(
                  resolution_ids, resolution_strings, node.resolution));
    py::list item;
    item.append(node.id);
    item.append(node.player);
    item.append(node.depth);
    item.append(kind_index);
    item.append(resolution_index);
    item.append(edge_start);
    item.append(edge_count - edge_start);
    nodes.append(item);
  }

  compact["kind_strings"] = kind_strings;
  compact["resolution_strings"] = resolution_strings;
  compact["action_templates"] = action_templates;
  compact["reveal_groups"] = reveal_groups;
  compact["nodes"] = nodes;
  compact["edges"] = edges;
  return compact;
}

} // namespace

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
    return copy_mask(result);
  }

  std::array<uint8_t, MAX_ACTIONS> get_action_mask_owned(Game &&game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("get_action_mask")(
        py::cast(std::move(game), py::return_value_policy::move));
    return copy_mask(result);
  }

private:
  static std::array<uint8_t, MAX_ACTIONS> copy_mask(const py::object &result) {
    py::array_t<uint8_t> arr = result.cast<py::array_t<uint8_t>>();
    std::array<uint8_t, MAX_ACTIONS> mask = {0};
    auto r = arr.unchecked<1>();
    for (ssize_t i = 0;
         i < std::min(static_cast<ssize_t>(MAX_ACTIONS), r.shape(0)); ++i) {
      mask[i] = r(i);
    }
    return mask;
  }

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
      .def_property(
          "bank", [](const Board &b) { return b.bank; },
          [](Board &b, std::array<uint8_t, 6> bank) {
            b.bank = bank;
            b.invalidate_hash();
          })
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
          [](Board &b, const std::vector<std::vector<int>> &visible) {
            if (visible.size() != 3)
              throw py::value_error("visible must have 3 levels");
            // Validate the complete payload before mutating Board.  Otherwise
            // an invalid value in a later slot leaves an earlier slot changed
            // while the cached hash is still marked valid.
            for (int i = 0; i < 3; ++i) {
              if (visible[i].size() != Board::CARDS_PER_LEVEL)
                throw py::value_error("each visible level must have 4 slots");
              for (int j = 0; j < Board::CARDS_PER_LEVEL; ++j) {
                int card_id = visible[i][j];
                if (card_id != -1 && !is_valid_card_id(card_id))
                  throw py::value_error("visible contains invalid card id");
              }
            }
            for (int i = 0; i < 3; ++i) {
              for (int j = 0; j < Board::CARDS_PER_LEVEL; ++j) {
                int card_id = visible[i][j];
                b.visible[i][j] = static_cast<int8_t>(card_id);
              }
            }
            b.invalidate_hash();
          })
      .def_property(
          "nobles",
          [](const Board &b) {
            py::list nobles(b.nobles.size());
            for (size_t i = 0; i < b.nobles.size(); ++i)
              nobles[i] = static_cast<int>(b.nobles[i]);
            return nobles;
          },
          [](Board &b, const std::vector<int> &nobles) {
            if (nobles.size() > Board::MAX_NOBLES_ON_BOARD)
              throw py::value_error("too many nobles");
            for (int noble_id : nobles) {
              if (!is_valid_noble_id(noble_id))
                throw py::value_error("nobles contains invalid noble id");
            }
            b.nobles.clear();
            for (int noble_id : nobles) {
              b.nobles.push_back(static_cast<uint8_t>(noble_id));
            }
            b.invalidate_hash();
          })
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
          [](Board &b, const std::vector<std::vector<int>> &decks) {
            if (decks.size() != 3)
              throw py::value_error("decks must have 3 levels");
            for (int i = 0; i < 3; ++i) {
              if (decks[i].size() > Board::MAX_DECK_SIZE)
                throw py::value_error("deck level exceeds max size");
              for (int card_id : decks[i]) {
                if (!is_valid_card_id(card_id) ||
                    get_card(card_id).level != i + 1)
                  throw py::value_error("decks contains invalid card id");
              }
            }
            for (int i = 0; i < 3; ++i) {
              b.decks[i].clear();
              for (int card_id : decks[i]) {
                b.decks[i].push_back(static_cast<uint8_t>(card_id));
              }
            }
            b.invalidate_hash();
          })
      .def_property(
          "final_round", [](const Board &b) { return b.final_round; },
          [](Board &b, bool final_round) {
            b.final_round = final_round;
            b.invalidate_hash();
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
      .def("observable_repetition_hash", &Board::observable_repetition_hash,
           py::arg("observer"),
           "Observable position hash that ignores the monotonic turn counter")
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

  m.def(
      "solve_visible_only_winner_cpp",
      [](const Game &game, uint64_t max_nodes, double time_limit_seconds) {
        // Snapshot while the GIL still excludes Python-side Board mutation.
        // The solver releases the GIL and must not keep reading the caller's
        // live vectors concurrently with Python editor setters.
        Game input_snapshot = game.clone_light();
        VisibleOnlySearchResult result;
        {
          py::gil_scoped_release release;
          result = VisibleOnlySolver(max_nodes, time_limit_seconds)
                       .solve(input_snapshot);
        }

        py::dict stats;
        stats["nodes"] = result.stats.nodes;
        stats["memo_hits"] = result.stats.memo_hits;
        stats["terminal_nodes"] = result.stats.terminal_nodes;
        stats["legal_moves"] = result.stats.legal_moves;
        stats["elapsed_ms"] = result.stats.elapsed_ms;

        py::list line;
        for (const VisibleOnlyLineEntry &entry : result.line) {
          py::dict item;
          item["action_code"] = entry.action_code;
          item["winner"] = entry.winner;
          item["reason"] = entry.reason;
          item["action_count"] = entry.action_count;
          line.append(item);
        }

        py::dict payload;
        payload["winner"] = result.winner;
        payload["forced_win_depth"] =
            result.forced_win_depth < 0 ? py::none()
                                        : py::cast(result.forced_win_depth);
        payload["simple_payment_mode"] = result.simple_payment_mode;
        payload["winner_reason"] = result.winner_reason;
        payload["unknown_reason"] =
            result.unknown_reason.empty() ? py::none()
                                          : py::cast(result.unknown_reason);
        payload["memoized_states"] = result.memoized_states;
        payload["stats"] = stats;
        payload["line"] = line;
        return payload;
      },
      py::arg("game"), py::arg("max_nodes") = 0,
      py::arg("time_limit_seconds") = 0.0);

  m.def(
      "solve_reveal_verified_mate_cpp",
      [](const Game &game, int attacker, int depth, uint64_t max_nodes,
         double time_limit_seconds,
         const std::vector<uint64_t> &preferred_attacker_actions,
         bool include_proof_dag, size_t proof_dag_node_limit,
         size_t proof_dag_edge_limit, uint64_t required_root_action,
         bool strict_preferred_attacker_actions,
         size_t strict_preferred_attacker_prefix,
         const std::string &proof_dag_format) {
        if (proof_dag_format != "v1" && proof_dag_format != "compact") {
          throw std::invalid_argument(
              "proof_dag_format must be 'v1' or 'compact'");
        }
        // As above, detach all solver input before releasing the GIL.  The
        // reveal solver reads deck and provenance vectors during its initial
        // clone, so cloning only after release permits a Python data race.
        Game input_snapshot = game.clone_light();
        RevealVerifiedSearchResult result;
        {
          py::gil_scoped_release release;
          result = RevealVerifiedSolver(attacker, depth, max_nodes,
                                        time_limit_seconds,
                                        preferred_attacker_actions,
                                        include_proof_dag,
                                        proof_dag_node_limit,
                                        proof_dag_edge_limit,
                                        required_root_action,
                                        strict_preferred_attacker_actions,
                                        strict_preferred_attacker_prefix)
                       .solve(input_snapshot);
        }

        py::dict stats;
        stats["nodes"] = result.stats.nodes;
        stats["memo_hits"] = result.stats.memo_hits;
        stats["terminal_nodes"] = result.stats.terminal_nodes;
        stats["legal_moves"] = result.stats.legal_moves;
        stats["reveal_branches"] = result.stats.reveal_branches;
        stats["final_round_reveal_collapses"] =
            result.stats.final_round_reveal_collapses;
        stats["final_round_score_prunes"] =
            result.stats.final_round_score_prunes;
        stats["final_round_direct_resolutions"] =
            result.stats.final_round_direct_resolutions;
        stats["oracle_purchase_actions"] =
            result.stats.oracle_purchase_actions;
        stats["oracle_reserve_actions"] =
            result.stats.oracle_reserve_actions;
        stats["deck_reserve_candidates"] =
            result.stats.deck_reserve_candidates;
        stats["deck_reserve_branches"] = result.stats.deck_reserve_branches;
        stats["elapsed_ms"] = result.stats.elapsed_ms;

        py::list line;
        for (const RevealVerifiedLineEntry &entry : result.line) {
          py::dict item;
          item["action_code"] = entry.action_code;
          item["reveal_card"] =
              entry.reveal_card < 0 ? py::none() : py::cast(entry.reveal_card);
          item["action_count"] = entry.action_count;
          line.append(item);
        }

        py::dict payload;
        payload["proven"] = result.proven;
        payload["attacker"] = result.attacker;
        payload["depth"] = result.depth;
        payload["reason"] = result.reason;
        payload["unknown_reason"] =
            result.unknown_reason.empty() ? py::none()
                                          : py::cast(result.unknown_reason);
        payload["memoized_states"] = result.memoized_states;
        payload["stats"] = stats;
        payload["line"] = line;
        payload["proof_dag"] =
            proof_dag_format == "compact"
                ? proof_dag_to_py_compact(result.proof_dag)
                : proof_dag_to_py_v1(result.proof_dag);
        return payload;
      },
      py::arg("game"), py::arg("attacker"), py::arg("depth"),
      py::arg("max_nodes") = 0, py::arg("time_limit_seconds") = 0.0,
      py::arg("preferred_attacker_actions") = std::vector<uint64_t>{},
      py::arg("include_proof_dag") = false,
      py::arg("proof_dag_node_limit") = 100000,
      py::arg("proof_dag_edge_limit") = 500000,
      py::arg("required_root_action") = UINT64_MAX,
      py::arg("strict_preferred_attacker_actions") = false,
      py::arg("strict_preferred_attacker_prefix") = 0,
      py::arg("proof_dag_format") = "v1");

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

  // ActionEncoderV2 bindings (full 4869-action space with return + payment patterns)
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
  py::enum_<mcts_parallel::TreeBackend>(m, "ParallelTreeBackend")
      .value("COARSE", mcts_parallel::TreeBackend::Coarse)
      .value("SHARDED", mcts_parallel::TreeBackend::Sharded);

  py::enum_<mcts_parallel::ParallelSearchMode>(m, "ParallelSearchMode")
      .value("THROUGHPUT", mcts_parallel::ParallelSearchMode::Throughput)
      .value("DETERMINISTIC_EPOCH",
             mcts_parallel::ParallelSearchMode::DeterministicEpoch)
      .value("ROOT_PARALLEL",
             mcts_parallel::ParallelSearchMode::RootParallel);

  py::enum_<mcts_parallel::SearchStopReason>(m, "ParallelSearchStopReason")
      .value("COMPLETED", mcts_parallel::SearchStopReason::Completed)
      .value("CANCELLED", mcts_parallel::SearchStopReason::Cancelled)
      .value("TIMED_OUT", mcts_parallel::SearchStopReason::TimedOut)
      .value("TREE_CAPACITY_REACHED",
             mcts_parallel::SearchStopReason::TreeCapacityReached)
      .value("CALLBACK_ERROR",
             mcts_parallel::SearchStopReason::CallbackError)
      .value("WORKER_ERROR", mcts_parallel::SearchStopReason::WorkerError);

  py::class_<mcts_parallel::ParallelCancellationToken>(
      m, "ParallelCancellationToken")
      .def(py::init<>())
      .def("request_cancel",
           &mcts_parallel::ParallelCancellationToken::request_cancel)
      .def_property_readonly(
          "is_cancelled",
          &mcts_parallel::ParallelCancellationToken::is_cancelled);

  py::class_<mcts_parallel::ParallelSearchOptions>(m,
                                                    "ParallelSearchOptions")
      .def(py::init<>())
      .def_readwrite("num_threads",
                     &mcts_parallel::ParallelSearchOptions::num_threads)
      .def_readwrite("batch_size",
                     &mcts_parallel::ParallelSearchOptions::batch_size)
      .def_readwrite("batch_wait_us",
                     &mcts_parallel::ParallelSearchOptions::batch_wait_us)
      .def_readwrite("max_inflight",
                     &mcts_parallel::ParallelSearchOptions::max_inflight)
      .def_readwrite(
          "deterministic_epoch_size",
          &mcts_parallel::ParallelSearchOptions::deterministic_epoch_size)
      .def_readwrite("num_simulations",
                     &mcts_parallel::ParallelSearchOptions::num_simulations)
      .def_readwrite("master_seed",
                     &mcts_parallel::ParallelSearchOptions::master_seed)
      .def_readwrite("search_nonce",
                     &mcts_parallel::ParallelSearchOptions::search_nonce)
      .def_readwrite("simulation_id_base",
                     &mcts_parallel::ParallelSearchOptions::simulation_id_base)
      .def_readwrite("evaluator_version",
                     &mcts_parallel::ParallelSearchOptions::evaluator_version)
      .def_readwrite("timeout_ms",
                     &mcts_parallel::ParallelSearchOptions::timeout_ms)
      .def_readwrite("max_tree_nodes",
                     &mcts_parallel::ParallelSearchOptions::max_tree_nodes)
      .def_readwrite("shard_count",
                     &mcts_parallel::ParallelSearchOptions::shard_count)
      .def_readwrite("tree_backend",
                     &mcts_parallel::ParallelSearchOptions::tree_backend)
      .def_readwrite("mode", &mcts_parallel::ParallelSearchOptions::mode)
      .def_readwrite(
          "cancellation_token",
          &mcts_parallel::ParallelSearchOptions::cancellation_token);

  py::class_<mcts_parallel::SearchLedgerSnapshot>(
      m, "ParallelSearchLedger")
      .def_readonly("issued",
                    &mcts_parallel::SearchLedgerSnapshot::issued)
      .def_readonly("selected",
                    &mcts_parallel::SearchLedgerSnapshot::selected)
      .def_readonly("evaluation_owner",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_owner)
      .def_readonly("evaluation_waiter",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_waiter)
      .def_readonly("evaluation_requested",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_requested)
      .def_readonly("evaluated_boards",
                    &mcts_parallel::SearchLedgerSnapshot::evaluated_boards)
      .def_readonly(
          "completed_evaluated",
          &mcts_parallel::SearchLedgerSnapshot::completed_evaluated)
      .def_readonly("completed_terminal",
                    &mcts_parallel::SearchLedgerSnapshot::completed_terminal)
      .def_readonly("completed_max_depth",
                    &mcts_parallel::SearchLedgerSnapshot::completed_max_depth)
      .def_readonly("cancelled",
                    &mcts_parallel::SearchLedgerSnapshot::cancelled)
      .def_readonly("failed", &mcts_parallel::SearchLedgerSnapshot::failed)
      .def_readonly(
          "virtual_loss_added",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_added)
      .def_readonly(
          "virtual_loss_released",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_released)
      .def_readonly(
          "reservations_committed",
          &mcts_parallel::SearchLedgerSnapshot::reservations_committed)
      .def_readonly("reservations_aborted",
                    &mcts_parallel::SearchLedgerSnapshot::reservations_aborted)
      .def_readonly("expansion_claimed",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_claimed)
      .def_readonly("expansion_published",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_published)
      .def_readonly("expansion_waited",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_waited)
      .def_readonly("stale_result",
                    &mcts_parallel::SearchLedgerSnapshot::stale_result)
      .def_readonly("duplicate_result",
                    &mcts_parallel::SearchLedgerSnapshot::duplicate_result)
      .def_readonly("invalid_replay",
                    &mcts_parallel::SearchLedgerSnapshot::invalid_replay)
      .def_readonly("integrity_errors",
                    &mcts_parallel::SearchLedgerSnapshot::integrity_errors)
      .def_readonly(
          "max_inflight_observed",
          &mcts_parallel::SearchLedgerSnapshot::max_inflight_observed)
      .def_property_readonly("completed",
                             &mcts_parallel::SearchLedgerSnapshot::completed)
      .def_property_readonly(
          "virtual_loss_balanced",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_balanced);

  py::class_<mcts_parallel::ParallelSearchResult>(m, "ParallelSearchResult")
      .def_property_readonly(
          "visits", [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<uint64_t>(result.visits.begin(),
                                         result.visits.end());
          })
      .def_property_readonly(
          "q_values", [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<double>(result.q_values.begin(),
                                       result.q_values.end());
          })
      .def_property_readonly(
          "probabilities",
          [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<float>(result.probabilities.begin(),
                                      result.probabilities.end());
          })
      .def_readonly("ledger", &mcts_parallel::ParallelSearchResult::ledger)
      .def_readonly("stop_reason",
                    &mcts_parallel::ParallelSearchResult::stop_reason)
      .def_readonly("resolved_seed",
                    &mcts_parallel::ParallelSearchResult::resolved_seed)
      .def_readonly("rng_version",
                    &mcts_parallel::ParallelSearchResult::rng_version)
      .def_readonly("search_nonce",
                    &mcts_parallel::ParallelSearchResult::search_nonce)
      .def_readonly("tree_generation",
                    &mcts_parallel::ParallelSearchResult::tree_generation)
      .def_readonly("tree_size",
                    &mcts_parallel::ParallelSearchResult::tree_size)
      .def_readonly("elapsed_microseconds",
                    &mcts_parallel::ParallelSearchResult::elapsed_microseconds)
      .def_readonly("partial", &mcts_parallel::ParallelSearchResult::partial);

  py::class_<mcts_parallel::RootParallelResult>(m, "RootParallelSearchResult")
      .def_readonly("merged", &mcts_parallel::RootParallelResult::merged)
      .def_readonly("workers", &mcts_parallel::RootParallelResult::workers)
      .def_readonly(
          "duplicate_root_evaluations_avoided",
          &mcts_parallel::RootParallelResult::duplicate_root_evaluations_avoided);

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
      .def("reset_replay_sequence", &MCTS::reset_replay_sequence,
           py::arg("seed"), py::arg("nonce"),
           "Reset the parallel-search seed and next nonce while idle")
      .def("tree_size", &MCTS::tree_size)
      .def("prune_if_needed", &MCTS::prune_if_needed)
      .def("get_node",
           [](MCTS &mcts, uint64_t hash) -> py::object {
             auto node = mcts.get_node_snapshot(hash);
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

            // Convert to Python-friendly format. Every array remains an
            // independent owning copy; only list growth is eliminated.
            py::dict py_result;

            // Flatten encoded boards and valid actions for batch NN inference
            const size_t leaf_count = result.leaves.size();
            py::list flat_boards(result.total_boards);
            py::list flat_valids(result.total_boards);
            py::list leaf_world_counts(leaf_count);
            py::list leaf_hashes(leaf_count);
            py::list leaf_paths(leaf_count);
            size_t flat_index = 0;

            for (size_t leaf_index = 0; leaf_index < leaf_count;
                 ++leaf_index) {
              const auto &leaf = result.leaves[leaf_index];
              leaf_hashes[leaf_index] = leaf.hash;
              leaf_world_counts[leaf_index] = leaf.num_worlds;

              // Convert path to Python list
              py::list py_path(leaf.path.size());
              for (size_t path_index = 0; path_index < leaf.path.size();
                   ++path_index) {
                const auto &entry = leaf.path[path_index];
                py_path[path_index] =
                    py::make_tuple(entry.hash, entry.action, entry.player);
              }
              leaf_paths[leaf_index] = py_path;

              // Add boards and valid actions
              const size_t leaf_flat_start = flat_index;
              for (const auto &board : leaf.encoded_boards) {
                flat_boards[flat_index] = owning_array_copy(board);
                ++flat_index;
              }
              for (size_t valid_index = 0;
                   valid_index < leaf.valid_actions.size(); ++valid_index) {
                const auto &valid = leaf.valid_actions[valid_index];
                flat_valids[leaf_flat_start + valid_index] =
                    owning_array_copy(valid);
              }
            }

            // Handle terminals
            py::list py_terminals(result.terminals.size());
            for (size_t terminal_index = 0;
                 terminal_index < result.terminals.size(); ++terminal_index) {
              const auto &[path, value] = result.terminals[terminal_index];
              py::list py_path(path.size());
              for (size_t path_index = 0; path_index < path.size();
                   ++path_index) {
                const auto &entry = path[path_index];
                py_path[path_index] =
                    py::make_tuple(entry.hash, entry.action, entry.player);
              }
              py::list py_value(NUM_PLAYERS);
              for (size_t value_index = 0; value_index < NUM_PLAYERS;
                   ++value_index)
                py_value[value_index] = value[value_index];
              py_terminals[terminal_index] = py::make_tuple(py_path, py_value);
            }

            py_result["flat_boards"] = flat_boards;
            py_result["flat_valids"] = flat_valids;
            py_result["leaf_world_counts"] = leaf_world_counts;
            py_result["leaf_hashes"] = leaf_hashes;
            py_result["leaf_paths"] = leaf_paths;
            py_result["terminals"] = py_terminals;
            py_result["total_boards"] = result.total_boards;
            py_result["num_leaves"] = static_cast<int>(result.leaves.size());
            py_result["tree_generation"] = result.tree_generation;

            return py_result;
          },
          py::arg("root_game"), py::arg("observer"), py::arg("batch_size"),
          py::arg("num_determinizations"), py::arg("dirichlet_noise"),
          "Prepare batch simulations for NN evaluation (uses native C++ "
          "ActionEncoder)")
      .def(
          "apply_batch_results",
          [](MCTS &mcts, py::dict request, py::list policies, py::list values) {
            py::list leaf_hashes = request["leaf_hashes"].cast<py::list>();
            py::list leaf_world_counts =
                request["leaf_world_counts"].cast<py::list>();
            py::list leaf_paths = request["leaf_paths"].cast<py::list>();
            py::list flat_valids = request["flat_valids"].cast<py::list>();
            py::list terminals = request["terminals"].cast<py::list>();

            if (py::len(leaf_hashes) != py::len(leaf_world_counts) ||
                py::len(leaf_hashes) != py::len(leaf_paths))
              throw py::value_error("batch leaf metadata lengths differ");
            size_t expected_worlds = 0;
            for (auto item : leaf_world_counts) {
              const int count = item.cast<int>();
              if (count <= 0)
                throw py::value_error("leaf world count must be positive");
              expected_worlds += static_cast<size_t>(count);
            }
            if (py::len(policies) < expected_worlds ||
                py::len(values) < expected_worlds ||
                py::len(flat_valids) < expected_worlds)
              throw py::value_error(
                  "batch results do not match requested worlds");

            // Convert the complete Python payload first. The canonical native
            // implementation validates it once more before mutating any node,
            // so malformed/stale batches cannot be partially applied.
            BatchSimulationRequest native_request;
            native_request.tree_generation =
                request.contains("tree_generation")
                    ? request["tree_generation"].cast<uint64_t>()
                    : mcts.tree_generation_snapshot();
            native_request.total_boards = static_cast<int>(expected_worlds);
            size_t result_idx = 0;
            for (size_t i = 0; i < py::len(leaf_hashes); ++i) {
              BatchLeafData leaf;
              leaf.hash = leaf_hashes[i].cast<uint64_t>();
              leaf.num_worlds = leaf_world_counts[i].cast<int>();
              py::list py_path = leaf_paths[i].cast<py::list>();
              leaf.path.reserve(py::len(py_path));
              for (auto item : py_path) {
                py::tuple t = item.cast<py::tuple>();
                if (py::len(t) != 3)
                  throw py::value_error("batch path entry must have 3 fields");
                PathEntry entry;
                entry.hash = t[0].cast<uint64_t>();
                entry.action = t[1].cast<int>();
                entry.player = t[2].cast<int>();
                leaf.path.push_back(entry);
              }
              for (int world = 0; world < leaf.num_worlds; ++world) {
                py::array_t<uint8_t> valid =
                    flat_valids[result_idx].cast<py::array_t<uint8_t>>();
                auto valid_mask = valid.unchecked<1>();
                if (valid_mask.shape(0) < static_cast<ssize_t>(MAX_ACTIONS))
                  throw py::value_error("valid-action mask is too short");
                std::array<uint8_t, MAX_ACTIONS> mask{};
                for (size_t action = 0; action < MAX_ACTIONS; ++action)
                  mask[action] = valid_mask(static_cast<ssize_t>(action));
                leaf.valid_actions.push_back(mask);
                result_idx++;
              }
              native_request.leaves.push_back(std::move(leaf));
            }

            for (auto item : terminals) {
              py::tuple t = item.cast<py::tuple>();
              if (py::len(t) != 2)
                throw py::value_error("terminal batch entry must have 2 fields");
              py::list py_path = t[0].cast<py::list>();
              py::list py_value = t[1].cast<py::list>();
              std::vector<PathEntry> path;
              path.reserve(py::len(py_path));
              for (auto p_item : py_path) {
                py::tuple pt = p_item.cast<py::tuple>();
                if (py::len(pt) != 3)
                  throw py::value_error("batch path entry must have 3 fields");
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
              native_request.terminals.push_back({std::move(path), value});
            }

            std::vector<std::array<float, MAX_ACTIONS>> native_policies;
            std::vector<std::array<float, NUM_PLAYERS>> native_values;
            native_policies.reserve(expected_worlds);
            native_values.reserve(expected_worlds);
            for (size_t index = 0; index < expected_worlds; ++index) {
              py::array_t<float> policy =
                  policies[index].cast<py::array_t<float>>();
              py::array_t<float> value =
                  values[index].cast<py::array_t<float>>();
              auto policy_view = policy.unchecked<1>();
              auto value_view = value.unchecked<1>();
              if (policy_view.shape(0) < static_cast<ssize_t>(MAX_ACTIONS) ||
                  value_view.shape(0) < static_cast<ssize_t>(NUM_PLAYERS))
                throw py::value_error("batch policy/value array is too short");
              std::array<float, MAX_ACTIONS> policy_array{};
              std::array<float, NUM_PLAYERS> value_array{};
              for (size_t action = 0; action < MAX_ACTIONS; ++action)
                policy_array[action] =
                    policy_view(static_cast<ssize_t>(action));
              for (size_t player = 0; player < NUM_PLAYERS; ++player)
                value_array[player] =
                    value_view(static_cast<ssize_t>(player));
              native_policies.push_back(policy_array);
              native_values.push_back(value_array);
            }
            mcts.apply_batch_results(native_request, native_policies,
                                     native_values);
          },
          py::arg("request"), py::arg("policies"), py::arg("values"),
          "Apply batch NN results to the tree")
      .def("get_config_snapshot", &MCTS::get_config_snapshot)
      .def("set_config", &MCTS::set_config, py::arg("config"))
      .def("is_parallel_search_active", &MCTS::is_parallel_search_active)
      .def("tree_generation", &MCTS::tree_generation_snapshot)
      .def_property(
          "config", [](const MCTS &mcts) { return mcts.get_config_snapshot(); },
          [](MCTS &mcts, const MCTSConfig &config) { mcts.set_config(config); });

  m.def(
      "mcts_search_parallel_native",
      [](MCTS &mcts, const Game &root_game,
         const mcts_parallel::ParallelSearchOptions &options,
         py::function inference_fn, float temperature) {
        auto inference = make_python_parallel_inference(inference_fn);

        mcts_parallel::ParallelMCTSSearcher searcher;
        mcts_parallel::ParallelSearchResult result;
        // Snapshot every Python-owned input while the GIL is held. Acquiring
        // the search guard here also freezes config/generation at API entry,
        // before another Python thread can mutate the same MCTS/options.
        mcts_parallel::ParallelSearchOptions options_snapshot = options;
        mcts_parallel::ParallelMCTSSearcher::validate_entry_options(
            options_snapshot, inference, temperature);
        mcts_parallel::ParallelMCTSSearcher::validate_config_snapshot(
            mcts.get_config_snapshot());
        Game root_snapshot = root_game.clone_light();
        if (options_snapshot.num_simulations > 0 &&
            !options_snapshot.cancellation_requested() &&
            mcts_internal::GameAdapter::requires_forced_pass(root_snapshot))
          throw std::invalid_argument(
              "MCTS root requires a forced pass; apply it before searching");
        auto guard = mcts.begin_parallel_search();
        {
          py::gil_scoped_release release;
          result = searcher.run_with_guard(
              mcts, std::move(guard), root_snapshot, options_snapshot,
              inference, temperature);
        }
        return result;
      },
      py::arg("mcts"), py::arg("root_game"), py::arg("options"),
      py::arg("inference_fn"), py::arg("temperature") = 1.0f,
      "Experimental native parallel MCTS search. Traversal releases the GIL; "
      "one coordinator serializes Python inference callbacks.");

  m.def(
      "mcts_search_root_parallel_native",
      [](const MCTSConfig &config, const Game &root_game,
         uint64_t simulation_budget, uint32_t num_workers,
         const mcts_parallel::ParallelSearchOptions &options,
         py::function inference_fn, float temperature) {
        const MCTSConfig config_snapshot = config;
        mcts_parallel::ParallelSearchOptions options_snapshot = options;
        options_snapshot.serialize_root_callbacks = true;
        Game root_snapshot = root_game.clone_light();
        auto evaluator_factory =
            [&inference_fn](uint32_t /*worker_id*/) {
              return make_python_parallel_inference(inference_fn);
            };
        mcts_parallel::RootParallelResult result;
        {
          py::gil_scoped_release release;
          result = mcts_parallel::run_root_parallel(
              config_snapshot, root_snapshot, simulation_budget, num_workers,
              options_snapshot, evaluator_factory, temperature);
        }
        return result;
      },
      py::arg("config"), py::arg("root_game"),
      py::arg("simulation_budget"), py::arg("num_workers"),
      py::arg("options"), py::arg("inference_fn"),
      py::arg("temperature") = 1.0f,
      "Experimental independent-tree root-parallel fallback. Worker "
      "traversal releases the GIL; Python callbacks remain GIL-serialized.");

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
            res.policy.fill(0.0f);
            for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
              res.policy[i] = policy[i];
          })
      .def_property(
          "value",
          [](const InferenceResult &res) {
            return std::vector<float>(res.value.begin(), res.value.end());
          },
          [](InferenceResult &res, const std::vector<float> &value) {
            res.value.fill(0.0f);
            for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
              res.value[i] = value[i];
          });

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
        // Python inference callbacks can retain and mutate the caller's root
        // through a closure. Snapshot it while the GIL is still held so every
        // simulation and the final root lookup use one stable position.
        Game root_snapshot = mcts.config().use_determinization
                                 ? root_game.clone_light()
                                 : root_game.clone();

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
            // The callback may retain a request after it returns. Keep the
            // ndarray lifetime independent of the temporary C++ request
            // vector instead of exposing a dangling view.
            d["features"] = owning_array_copy(req.features);
            d["valid_actions"] = owning_array_copy(req.valid_actions);
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
          searcher.search(root_snapshot, num_simulations, cpp_inference);
        }

        // Get action probabilities
        auto probs = searcher.get_action_probs(root_snapshot, temperature);
        return std::vector<float>(probs.begin(), probs.end());
      },
      py::arg("mcts"), py::arg("featurizer"), py::arg("encoder"),
      py::arg("root_game"), py::arg("num_simulations"), py::arg("inference_fn"),
      py::arg("temperature"),
      "Run full MCTS search with C++ searcher and Python inference callback");
}
