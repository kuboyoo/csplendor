"""Regression contracts spanning domain, rules, encoding and search."""

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

import csplendor as cs

ROOT = Path(__file__).resolve().parents[1]


NATIVE_BATCH_VALIDATION = r'''
#include "mcts.h"
#include <array>
#include <stdexcept>
#include <vector>

int main() {
  MCTSConfig config;
  MCTS mcts(config);
  BatchSimulationRequest request;
  BatchLeafData leaf;
  leaf.hash = 7;
  leaf.num_worlds = 1;
  leaf.valid_actions.push_back({});
  request.leaves.push_back(leaf);

  bool short_results_rejected = false;
  try {
    mcts.apply_batch_results(request, {}, {});
  } catch (const std::invalid_argument &) {
    short_results_rejected = true;
  }

  request.leaves[0].num_worlds = 0;
  bool zero_worlds_rejected = false;
  try {
    mcts.apply_batch_results(request, {}, {});
  } catch (const std::invalid_argument &) {
    zero_worlds_rejected = true;
  }
  return short_results_rejected && zero_worlds_rejected ? 0 : 1;
}
'''


def _mcts_root_key(game, determinization):
    config = cs.MCTSConfig()
    config.use_determinization = determinization
    config.use_dirichlet_noise = False
    mcts = cs.MCTS(config)
    request = mcts.prepare_batch_simulations(game, 0, 1, 1, None)
    assert request["num_leaves"] == 1
    return int(request["leaf_hashes"][0])


def test_mcts_tree_key_distinguishes_game_modes_without_changing_default_key():
    for determinization in (False, True):
        default = cs.Game(seed=73)
        expected_default = (
            default.board.observable_hash(0)
            if determinization
            else default.board_hash()
        )
        default_key = _mcts_root_key(default, determinization)
        assert default_key == expected_default

        simple = default.clone_light()
        simple.simple_payment_mode = True
        blank = default.clone_light()
        blank.blank_refill_mode = True
        both = default.clone_light()
        both.simple_payment_mode = True
        both.blank_refill_mode = True

        keys = {
            default_key,
            _mcts_root_key(simple, determinization),
            _mcts_root_key(blank, determinization),
            _mcts_root_key(both, determinization),
        }
        assert len(keys) == 4


def test_native_batch_result_validation_rejects_incomplete_payloads():
    compiler = os.environ.get("CXX", "c++")
    if not shutil.which(compiler):
        pytest.skip(f"C++ compiler is unavailable: {compiler}")
    with tempfile.TemporaryDirectory(prefix="csplendor-review-") as directory:
        temporary = Path(directory)
        source = temporary / "batch_validation.cpp"
        binary = temporary / "batch_validation"
        source.write_text(NATIVE_BATCH_VALIDATION)
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
        subprocess.run([binary], check=True)


def test_action_repr_names_noble_and_invalid_actions():
    noble = cs.Action()
    noble.type = cs.ActionType.VISIT_NOBLE
    noble.noble_choice = 3
    assert repr(noble) == "VISIT_NOBLE: N3"
    assert repr(cs.Action()) == "INVALID"
