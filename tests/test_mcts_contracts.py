"""MCTS facade, tree split, and Game adapter contracts."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pytest

from csplendor import MCTS, Game, MCTSConfig

ROOT = Path(__file__).resolve().parents[1]


NATIVE_CONTRACT_PROBE = r"""
#include "mcts_tree.h"

// The tree/statistics layer must remain independent of rule-engine headers.
#ifdef CSPLENDOR_GAME_H
#error "mcts_tree.h must not include game.h"
#endif
#ifdef CSPLENDOR_STATE_ENCODER_H
#error "mcts_tree.h must not include state_encoder.h"
#endif
#ifdef CSPLENDOR_ACTION_ENCODER_H
#error "mcts_tree.h must not include action_encoder.h"
#endif

#include "mcts.h"
#include "mcts_game_adapter.h"
#include <cstddef>
#include <iostream>
#include <type_traits>

using BatchSignature = BatchSimulationRequest (MCTS::*)(
    const Game &, uint8_t, int, int,
    const std::array<float, MAX_ACTIONS> *);
using LegacyBatchSignature = BatchSimulationRequest (MCTS::*)(
    const Game &, uint8_t, int, int,
    const std::array<float, MAX_ACTIONS> *, IActionEncoder &);
using SelectSignature = int (MCTS::*)(const MCTSNode &, bool, int);
using GetNodeSignature = MCTSNode *(MCTS::*)(uint64_t);
using ConfigSignature = const MCTSConfig &(MCTS::*)() const;

static_assert(std::is_standard_layout<MCTSNode>::value);
static_assert(std::is_same<
              decltype(static_cast<BatchSignature>(
                  &MCTS::prepare_batch_simulations)),
              BatchSignature>::value);
static_assert(std::is_same<
              decltype(static_cast<LegacyBatchSignature>(
                  &MCTS::prepare_batch_simulations)),
              LegacyBatchSignature>::value);
static_assert(std::is_same<decltype(&MCTS::select_action),
                           SelectSignature>::value);
static_assert(std::is_same<decltype(&MCTS::get_node),
                           GetNodeSignature>::value);
static_assert(std::is_same<
              decltype(static_cast<ConfigSignature>(&MCTS::config)),
              ConfigSignature>::value);

static bool transition_matches(ActionType type) {
  Game state(42);
  if (type == PURCHASE || type == VISIT_NOBLE) {
    PlayerState &player = state.board.players[state.board.current_player];
    player.bonuses = {10, 10, 10, 10, 10};
    player.sync_packed();
    state.board.waiting_noble = (type == VISIT_NOBLE);
    state.board.invalidate_hash();
  }

  int action_index = -1;
  for (const Action &action : state.legal_actions()) {
    if (action.type != type) continue;
    action_index = ActionEncoderCpp::encode(action, state);
    if (action_index >= 0) break;
  }
  if (action_index < 0) return false;

  Game direct = state.clone_light();
  Game adapted = state.clone_light();
  Action decoded = ActionEncoderCpp::decode(action_index, direct);
  const bool direct_ok = direct.apply_trusted(decoded, false);
  const bool adapted_ok =
      mcts_internal::GameAdapter::decode_and_apply_native(adapted,
                                                          action_index);
  return direct_ok == adapted_ok &&
         direct.board.compute_hash_uncached() ==
             adapted.board.compute_hash_uncached() &&
         direct.simple_payment_mode == adapted.simple_payment_mode &&
         direct.blank_refill_mode == adapted.blank_refill_mode;
}

int main() {
  Game root(42);
  const uint64_t action_code = root.legal_action_code_at(0);
  if (!root.apply_action_code(action_code, true)) return 10;

  Game full = mcts_internal::GameAdapter::clone_with_history(root);
  Game light = mcts_internal::GameAdapter::clone_light(root);
  const bool full_undo = full.undo();
  const bool light_undo = light.undo();

  Game direct = mcts_internal::GameAdapter::clone_light(root);
  Game adapted = mcts_internal::GameAdapter::clone_light(root);
  const auto mask = mcts_internal::GameAdapter::native_action_mask(adapted);
  int action_index = -1;
  for (size_t index = 0; index < mask.size(); ++index) {
    if (mask[index]) {
      action_index = static_cast<int>(index);
      break;
    }
  }
  if (action_index < 0) return 11;
  Action decoded = ActionEncoderCpp::decode(action_index, direct);
  if (!direct.apply_trusted(decoded, false)) return 12;
  if (!mcts_internal::GameAdapter::decode_and_apply_native(adapted,
                                                            action_index))
    return 13;

  const auto direct_features = StateEncoder::encode_canonical(root, 0, 0);
  const auto adapted_features =
      mcts_internal::GameAdapter::native_features(root, 0);
  const auto direct_mask = ActionEncoderCpp::get_action_mask(root);

  Game draw(7);
  draw.board.winner = -2;
  const auto batch_draw =
      mcts_internal::GameAdapter::terminal_value(draw, 0.0f);
  const auto search_draw =
      mcts_internal::GameAdapter::terminal_value(draw, 0.01f);

  const std::array<ActionType, 6> action_types = {
      TAKE_DIFFERENT, TAKE_SAME, RESERVE_VISIBLE,
      RESERVE_DECK, PURCHASE, VISIT_NOBLE};
  bool all_action_types_equal = true;
  for (ActionType type : action_types)
    all_action_types_equal = all_action_types_equal && transition_matches(type);

  std::cout
      << "{\"sizeof_node\":" << sizeof(MCTSNode)
      << ",\"align_node\":" << alignof(MCTSNode)
      << ",\"offsets\":["
      << offsetof(MCTSNode, valid_actions) << ","
      << offsetof(MCTSNode, prior) << ","
      << offsetof(MCTSNode, Q) << ","
      << offsetof(MCTSNode, N) << ","
      << offsetof(MCTSNode, virtual_loss) << ","
      << offsetof(MCTSNode, total_visits) << ","
      << offsetof(MCTSNode, value) << ","
      << offsetof(MCTSNode, is_terminal) << ","
      << offsetof(MCTSNode, is_expanded) << "]"
      << ",\"sizeof_config\":" << sizeof(MCTSConfig)
      << ",\"sizeof_path\":" << sizeof(PathEntry)
      << ",\"sizeof_leaf\":" << sizeof(LeafRequest)
      << ",\"sizeof_mcts\":" << sizeof(MCTS)
      << ",\"full_undo\":" << full_undo
      << ",\"light_undo\":" << light_undo
      << ",\"transition_equal\":"
      << (direct.board.hash() == adapted.board.hash())
      << ",\"features_equal\":"
      << (direct_features == adapted_features)
      << ",\"mask_equal\":" << (direct_mask == mask)
      << ",\"all_action_types_equal\":" << all_action_types_equal
      << ",\"batch_draw\":[" << batch_draw[0] << ","
      << batch_draw[1] << "]"
      << ",\"search_draw\":[" << search_draw[0] << ","
      << search_draw[1] << "]}"
      << std::endl;
}
"""


def _compiler():
    compiler = os.environ.get("CXX", "c++")
    if not shutil.which(compiler):
        pytest.skip(f"C++ compiler is unavailable: {compiler}")
    return compiler


def test_internal_tree_header_is_game_independent_and_facade_keeps_layout_and_api():
    compiler = _compiler()
    with tempfile.TemporaryDirectory(prefix="csplendor-mcts-") as directory:
        temporary = Path(directory)
        source = temporary / "mcts_contract.cpp"
        binary = temporary / "mcts_contract"
        source.write_text(NATIVE_CONTRACT_PROBE)
        subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-O2",
                f"-I{ROOT / 'src'}",
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            cwd=ROOT,
        )
        result = json.loads(subprocess.check_output([binary], text=True))

    # MCTS contains standard-library unordered_map/mt19937 objects, so its
    # exact size is measurement metadata rather than a cross-toolchain ABI
    # contract. MCTSNode and the plain request/path types are fixed below.
    assert result.pop("sizeof_mcts") >= result["sizeof_config"]
    assert result == {
        "sizeof_node": 832,
        "align_node": 4,
        "offsets": [0, 48, 240, 432, 624, 816, 820, 828, 829],
        "sizeof_config": 36,
        "sizeof_path": 16,
        "sizeof_leaf": 848,
        "full_undo": 1,
        "light_undo": 0,
        "transition_equal": 1,
        "features_equal": 1,
        "mask_equal": 1,
        "all_action_types_equal": 1,
        "batch_draw": [0, 0],
        "search_draw": [0.01, 0.01],
    }


def _batch_digest(request):
    metadata = {
        "leaf_hashes": [int(value) for value in request["leaf_hashes"]],
        "leaf_world_counts": [
            int(value) for value in request["leaf_world_counts"]
        ],
        "leaf_paths": [
            [tuple(map(int, entry)) for entry in path]
            for path in request["leaf_paths"]
        ],
        "terminals": [
            (
                [tuple(map(int, entry)) for entry in path],
                list(map(float, value)),
            )
            for path, value in request["terminals"]
        ],
        "total_boards": int(request["total_boards"]),
        "num_leaves": int(request["num_leaves"]),
    }
    digest = hashlib.sha256(
        json.dumps(
            metadata, sort_keys=True, separators=(",", ":")
        ).encode()
    )
    for board in request["flat_boards"]:
        digest.update(np.asarray(board, dtype=np.float32).tobytes())
    for mask in request["flat_valids"]:
        digest.update(np.asarray(mask, dtype=np.uint8).tobytes())
    return digest.hexdigest()


def test_batch_hash_mask_and_path_behavior():
    config = MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    mcts = MCTS(config)
    root = Game(seed=42)
    root_hash = root.board_hash()

    first = mcts.prepare_batch_simulations(root, 0, 3, 1, None)
    assert _batch_digest(first) == (
        "e0eac54b611e35f9bbdcd456133a111dfd648b23ea70319c3f4f0e0404c7bcca"
    )
    assert first["leaf_hashes"] == [root_hash] * 3
    assert first["leaf_paths"] == [[], [], []]

    policies = [
        np.arange(1, 49, dtype=np.float32)
        for _ in first["flat_boards"]
    ]
    values = [
        np.array([0.25, -0.25], dtype=np.float32)
        for _ in first["flat_boards"]
    ]
    mcts.apply_batch_results(first, policies, values)

    # Legality comes from the request masks, not from positive NN policy
    # entries. The four simulations therefore advance through legal root
    # actions and encode the reached leaf exactly once.
    second = mcts.prepare_batch_simulations(root, 0, 4, 1, None)
    assert _batch_digest(second) == (
        "87d74c4eb6a1ec96b4781c11b223018c20aa21ffc99e605cf0a9661e9d01d77d"
    )
    assert second["leaf_paths"] == [
        [(root_hash, 29, 0)],
        [(root_hash, 28, 0)],
        [(root_hash, 27, 0)],
        [(root_hash, 26, 0)],
    ]
    assert second["leaf_world_counts"] == [1, 1, 1, 1]
    assert second["terminals"] == []
    assert root.board_hash() == root_hash


def test_tree_statistics_digest_is_unchanged_through_facade():
    config = MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    mcts = MCTS(config)
    state_hash = 0xA11CE
    policy = np.zeros(48, dtype=np.float32)
    policy[3] = 0.25
    policy[7] = 0.75
    valid = np.zeros(48, dtype=np.uint8)
    valid[[3, 7]] = 1

    mcts.expand_node(state_hash, policy, [0.2, -0.2], valid)
    assert mcts.select_action_with_virtual_loss(
        state_hash, False, None, 0
    ) == 7
    for value in (0.5, -0.25, 1.0):
        mcts.update_stats(state_hash, 7, value)
    mcts.add_virtual_loss(state_hash, 3)

    node = mcts.get_node(state_hash)
    assert node.total_visits == 3
    assert node.N[7] == 3
    assert node.Q[7] == pytest.approx(5.0 / 12.0)
    assert node.virtual_loss[3] == 1
    np.testing.assert_array_equal(
        mcts.get_action_probs(state_hash, 1.0),
        np.array([0.0] * 7 + [1.0] + [0.0] * 40),
    )

    mcts.clear_virtual_losses()
    assert mcts.get_node(state_hash).virtual_loss[3] == 0
    mcts.clear()
    assert mcts.tree_size() == 0
