#include "bindings.h"
#include "card_data.h"
#include "game.h"
#include "reveal_verified_solver.h"
#include "visible_only_solver.h"
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace {

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

namespace csplendor::python {

void bind_solvers(py::module_ &m) {
  using csplendor::solver_internal::RevealSearchCancellationToken;
  py::class_<RevealSearchCancellationToken,
             std::shared_ptr<RevealSearchCancellationToken>>(
      m, "MateSearchCancellationToken")
      .def(py::init<std::shared_ptr<RevealSearchCancellationToken>>(),
           py::arg("parent") = nullptr)
      .def("request_cancel",
           &RevealSearchCancellationToken::request_cancel)
      .def("reset", &RevealSearchCancellationToken::reset)
      .def_property_readonly("is_cancelled",
                             &RevealSearchCancellationToken::is_cancelled);

  py::class_<RevealVerifiedSolver>(m, "NativeMateSearchSession")
      .def(
          py::init([](int attacker) {
            if (attacker < 0 || attacker >= Board::NUM_PLAYERS)
              throw std::invalid_argument("attacker must be 0 or 1");
            return RevealVerifiedSolver(attacker, 0, 0, 0.0, {}, false, 0, 0,
                                        UINT64_MAX, false, 0, true, true);
          }),
          py::arg("attacker"))
      .def(
          "search",
          [](RevealVerifiedSolver &session, const Game &game, int depth,
             uint64_t max_nodes, double time_limit_seconds,
             const std::vector<uint64_t> &preferred_attacker_actions,
             const std::shared_ptr<RevealSearchCancellationToken>
                 &cancellation_token,
             size_t max_cache_states) {
            if (depth < 0)
              throw std::invalid_argument("depth must be non-negative");
            Game input_snapshot = game.clone_light();
            RevealVerifiedSearchResult result;
            {
              py::gil_scoped_release release;
              result = session.solve_reusing_exact_cache(
                  input_snapshot, depth, max_nodes, time_limit_seconds,
                  preferred_attacker_actions, cancellation_token,
                  max_cache_states);
            }

            py::dict stats;
            stats["nodes"] = result.stats.nodes;
            stats["memo_hits"] = result.stats.memo_hits;
            stats["persistent_memo_hits"] =
                result.stats.persistent_memo_hits;
            stats["iterative_order_hits"] =
                result.stats.iterative_order_hits;
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
            stats["deck_reserve_branches"] =
                result.stats.deck_reserve_branches;
            stats["elapsed_ms"] = result.stats.elapsed_ms;

            py::list line;
            for (const RevealVerifiedLineEntry &entry : result.line) {
              py::dict item;
              item["action_code"] = entry.action_code;
              item["reveal_card"] = entry.reveal_card < 0
                                          ? py::none()
                                          : py::cast(entry.reveal_card);
              item["action_count"] = entry.action_count;
              line.append(item);
            }

            py::dict payload;
            payload["proven"] = result.proven;
            payload["attacker"] = result.attacker;
            payload["depth"] = result.depth;
            payload["reason"] = result.reason;
            payload["unknown_reason"] = result.unknown_reason.empty()
                                            ? py::none()
                                            : py::cast(result.unknown_reason);
            payload["memoized_states"] = result.memoized_states;
            payload["stats"] = stats;
            payload["line"] = line;
            return payload;
          },
          py::arg("game"), py::arg("depth"), py::arg("max_nodes") = 0,
          py::arg("time_limit_seconds") = 0.0,
          py::arg("preferred_attacker_actions") =
              std::vector<uint64_t>{},
          py::arg("cancellation_token") = nullptr,
          py::arg("max_cache_states") = 0)
      .def("clear", &RevealVerifiedSolver::clear_exact_cache)
      .def("trim", &RevealVerifiedSolver::trim_exact_cache,
           py::arg("max_cache_states"))
      .def_property_readonly("memoized_states",
                             &RevealVerifiedSolver::exact_cache_size);

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
      "solve_reveal_verified_frontier_cpp",
      [](const Game &game, int attacker, int depth, uint64_t max_nodes,
         double time_limit_seconds, size_t edge_limit,
         const std::vector<uint64_t> &preferred_attacker_actions) {
        if (attacker < 0 || attacker >= Board::NUM_PLAYERS)
          throw std::invalid_argument("attacker must be 0 or 1");
        if (depth < 0)
          throw std::invalid_argument("depth must be non-negative");
        Game input_snapshot = game.clone_light();
        RevealVerifiedFrontierResult result;
        {
          py::gil_scoped_release release;
          result = RevealVerifiedSolver(attacker, depth, max_nodes,
                                        time_limit_seconds,
                                        preferred_attacker_actions)
                       .expand_frontier(input_snapshot, edge_limit);
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
        stats["oracle_purchase_actions"] = result.stats.oracle_purchase_actions;
        stats["oracle_reserve_actions"] = result.stats.oracle_reserve_actions;
        stats["deck_reserve_candidates"] = result.stats.deck_reserve_candidates;
        stats["deck_reserve_branches"] = result.stats.deck_reserve_branches;
        stats["elapsed_ms"] = result.stats.elapsed_ms;

        py::list edges;
        for (const RevealVerifiedFrontierEdge &edge : result.edges) {
          py::dict item;
          item["action_code"] = edge.action_code;
          item["reveal_card"] =
              edge.reveal_card < 0 ? py::none() : py::cast(edge.reveal_card);
          item["child_depth"] = edge.child_depth;
          item["child_game"] =
              py::cast(edge.child, py::return_value_policy::copy);
          edges.append(item);
        }

        py::dict payload;
        payload["proven"] = result.proven;
        payload["complete"] = result.complete;
        payload["attacker"] = result.attacker;
        payload["depth"] = result.depth;
        payload["player"] = result.player;
        payload["winner"] = result.winner;
        payload["waiting_noble"] = result.waiting_noble;
        payload["kind"] = result.kind;
        payload["resolution"] = result.resolution.empty()
                                    ? py::none()
                                    : py::cast(result.resolution);
        payload["reason"] = result.reason;
        payload["unknown_reason"] = result.unknown_reason.empty()
                                        ? py::none()
                                        : py::cast(result.unknown_reason);
        payload["memoized_states"] = result.memoized_states;
        payload["stats"] = stats;
        payload["edges"] = edges;
        return payload;
      },
      py::arg("game"), py::arg("attacker"), py::arg("depth"),
      py::arg("max_nodes") = 0, py::arg("time_limit_seconds") = 0.0,
      py::arg("edge_limit") = 250000,
      py::arg("preferred_attacker_actions") = std::vector<uint64_t>{});

  m.def(
      "solve_reveal_verified_root_split_cpp",
      [](const Game &game, int attacker, int depth,
         uint64_t required_root_action, bool exhaustive_attacker_actions,
         size_t edge_limit,
         const std::shared_ptr<RevealSearchCancellationToken>
             &cancellation_token) {
        if (attacker < 0 || attacker >= Board::NUM_PLAYERS)
          throw std::invalid_argument("attacker must be 0 or 1");
        if (depth < 0)
          throw std::invalid_argument("depth must be non-negative");
        Game input_snapshot = game.clone_light();
        RevealVerifiedFrontierResult result;
        {
          py::gil_scoped_release release;
          result =
              RevealVerifiedSolver(
                  attacker, depth, 0, 0.0, {}, false, 0, 0,
                  required_root_action, false, 0,
                  exhaustive_attacker_actions, true, cancellation_token)
                  .split_root(input_snapshot, edge_limit);
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
        stats["oracle_purchase_actions"] = result.stats.oracle_purchase_actions;
        stats["oracle_reserve_actions"] = result.stats.oracle_reserve_actions;
        stats["deck_reserve_candidates"] =
            result.stats.deck_reserve_candidates;
        stats["deck_reserve_branches"] = result.stats.deck_reserve_branches;
        stats["elapsed_ms"] = result.stats.elapsed_ms;

        py::list edges;
        for (const RevealVerifiedFrontierEdge &edge : result.edges) {
          py::dict item;
          item["action_code"] = edge.action_code;
          item["reveal_card"] =
              edge.reveal_card < 0 ? py::none() : py::cast(edge.reveal_card);
          item["child_depth"] = edge.child_depth;
          item["child_game"] =
              py::cast(edge.child, py::return_value_policy::copy);
          edges.append(item);
        }

        py::dict payload;
        payload["complete"] = result.complete;
        payload["attacker"] = result.attacker;
        payload["depth"] = result.depth;
        payload["player"] = result.player;
        payload["winner"] = result.winner;
        payload["waiting_noble"] = result.waiting_noble;
        payload["kind"] = result.kind;
        payload["resolution"] = result.resolution.empty()
                                    ? py::none()
                                    : py::cast(result.resolution);
        payload["reason"] = result.reason;
        payload["unknown_reason"] = result.unknown_reason.empty()
                                        ? py::none()
                                        : py::cast(result.unknown_reason);
        payload["stats"] = stats;
        payload["edges"] = edges;
        return payload;
      },
      py::arg("game"), py::arg("attacker"), py::arg("depth"),
      py::arg("required_root_action") = UINT64_MAX,
      py::arg("exhaustive_attacker_actions") = true,
      py::arg("edge_limit") = 250000,
      py::arg("cancellation_token") = nullptr);

  m.def(
      "solve_reveal_verified_mate_cpp",
      [](const Game &game, int attacker, int depth, uint64_t max_nodes,
         double time_limit_seconds,
         const std::vector<uint64_t> &preferred_attacker_actions,
         bool include_proof_dag, size_t proof_dag_node_limit,
         size_t proof_dag_edge_limit, uint64_t required_root_action,
         bool strict_preferred_attacker_actions,
         size_t strict_preferred_attacker_prefix,
         const std::string &proof_dag_format,
         bool exhaustive_attacker_actions, bool exact_reveal_search,
         const std::shared_ptr<RevealSearchCancellationToken>
             &cancellation_token) {
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
                                        strict_preferred_attacker_prefix,
                                        exhaustive_attacker_actions,
                                        exact_reveal_search,
                                        cancellation_token)
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
      py::arg("proof_dag_format") = "v1",
      py::arg("exhaustive_attacker_actions") = false,
      py::arg("exact_reveal_search") = false,
      py::arg("cancellation_token") = nullptr);
}

} // namespace csplendor::python
