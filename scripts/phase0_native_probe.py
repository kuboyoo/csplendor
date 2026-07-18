#!/usr/bin/env python3
"""Build and run Phase 0 C++-only diagnostics outside the production module.

It measures Game/Board layout and full/light clone allocation behaviour, then
builds temporary instrumented headers that count attempted post-cap inserts.
Neither probe changes src/ nor the extension binary.

    python scripts/phase0_native_probe.py --output /tmp/csplendor-phase0-native.json
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


COPY_PROBE = r'''
#include "game.h"
#include "mcts.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

static std::atomic<unsigned long long> allocations{0};
void *operator new(std::size_t n) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

static Game with_history(int plies) {
  Game game(42);
  for (int ply = 0; ply < plies && !game.is_game_over(); ++ply) {
    MoveList actions = MoveGenerator::generate_all_fixed(game.board, game.simple_payment_mode);
    if (actions.empty()) break;
    game.apply_trusted(actions[ply % actions.size()], true);
  }
  return game;
}

int main() {
  std::cout << "{\"sizes\":{\"Action\":" << sizeof(Action)
            << ",\"MoveList\":" << sizeof(MoveList)
            << ",\"PlayerState\":" << sizeof(PlayerState)
            << ",\"Board\":" << sizeof(Board)
            << ",\"Game\":" << sizeof(Game)
            << ",\"MCTSNode\":" << sizeof(MCTSNode) << "},\"copies\":{";
  for (int plies : {0, 50, 200}) {
    Game game = with_history(plies);
    allocations.store(0, std::memory_order_relaxed);
    Game full = game.clone();
    auto full_allocations = allocations.load(std::memory_order_relaxed);
    allocations.store(0, std::memory_order_relaxed);
    Game light = game.clone_light();
    auto light_allocations = allocations.load(std::memory_order_relaxed);
    std::cout << "\"" << plies << "\":{\"requested_plies\":" << plies
              << ",\"retained_history\":" << game.history.size()
              << ",\"full_allocations\":" << full_allocations
              << ",\"light_allocations\":" << light_allocations << "}";
    if (plies != 200) std::cout << ",";
  }
  std::cout << "}}\n";
}
'''


OVERFLOW_PROBE = r'''
#include "game.h"
#include <iostream>

int main() {
  unsigned long long maximum_retained = 0;
  unsigned long long reachable_attempts_before = phase0_movelist_overflow_attempts;
  for (unsigned seed = 0; seed < 128; ++seed) {
    Game game(seed);
    for (int ply = 0; ply < 100 && !game.is_game_over(); ++ply) {
      MoveList actions = MoveGenerator::generate_all_fixed(game.board, game.simple_payment_mode);
      if (actions.size() > maximum_retained) maximum_retained = actions.size();
      if (actions.empty()) break;
      game.apply_trusted(actions[(seed + ply * 17) % actions.size()], false);
    }
  }
  unsigned long long reachable_overflow_attempts =
      phase0_movelist_overflow_attempts - reachable_attempts_before;

  Game editor(42);
  editor.board.players[0].gems = {3, 3, 3, 3, 3, 3};
  editor.board.players[0].sync_packed();
  unsigned long long editor_attempts_before = phase0_movelist_overflow_attempts;
  MoveList editor_actions = MoveGenerator::generate_all_fixed(
      editor.board, editor.simple_payment_mode);
  unsigned long long editor_overflow_attempts =
      phase0_movelist_overflow_attempts - editor_attempts_before;

  std::cout << "{\"reachable_random_corpus\":{\"max_retained\":" << maximum_retained
            << ",\"overflow_attempts\":" << reachable_overflow_attempts
            << "},\"editor_boundary\":{\"retained\":" << editor_actions.size()
            << ",\"overflow_attempts\":" << editor_overflow_attempts << "}}\n";
}
'''


def _compiler():
    compiler = os.environ.get("CXX", "g++")
    if not shutil.which(compiler):
        raise RuntimeError(f"C++ compiler not found: {compiler}")
    return compiler


def _compile(compiler, source, output, include_dirs):
    command = [compiler, "-std=c++17", "-O3", *[f"-I{path}" for path in include_dirs], str(source), "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    compiler = _compiler()
    with tempfile.TemporaryDirectory(prefix="csplendor-phase0-") as directory:
        temporary = Path(directory)
        copy_source = temporary / "copy_probe.cpp"
        copy_binary = temporary / "copy_probe"
        copy_source.write_text(COPY_PROBE)
        _compile(compiler, copy_source, copy_binary, [ROOT / "src"])
        copy_result = json.loads(subprocess.check_output([copy_binary], text=True))

        # Shadow only action.h.  This is an instrumented test build: on a full
        # MoveList it increments a counter rather than changing retained data.
        action_text = (ROOT / "src" / "action.h").read_text()
        original = "if (count < MAX_MOVES)\n      data[count++] = a;"
        replacement = (
            "if (count < MAX_MOVES)\n      data[count++] = a;\n"
            "    else\n      ++phase0_movelist_overflow_attempts;"
        )
        if original not in action_text:
            raise RuntimeError("MoveList::push_back layout changed; update Phase 0 probe")
        action_text = action_text.replace(
            "struct MoveList {",
            "inline unsigned long long phase0_movelist_overflow_attempts = 0;\n\nstruct MoveList {",
        ).replace(original, replacement)
        (temporary / "action.h").write_text(action_text)
        # Quoted includes first search the directory of the including header.
        # Shadow these two direct consumers as well, otherwise src/game.h would
        # resolve src/action.h before the temporary instrumented header.
        (temporary / "game.h").write_text((ROOT / "src" / "game.h").read_text())
        move_generator_text = (ROOT / "src" / "move_generator.h").read_text()
        # Phase 2 stops the emitter as soon as the retained prefix reaches
        # MAX_MOVES.  In the temporary probe only, keep enumerating the same
        # capped base prefix and count final actions that would have been
        # offered to the legacy MoveList after it became full.
        phase2_limit = '''if (emitted_count >= MAX_MOVES)
        return false;
      ++emitted_count;
      return sink(action) && emitted_count < MAX_MOVES;'''
        phase2_probe = '''if (emitted_count >= MAX_MOVES) {
        ++phase0_movelist_overflow_attempts;
        return true;
      }
      ++emitted_count;
      (void)sink(action);
      return true;'''
        if phase2_limit in move_generator_text:
            move_generator_text = move_generator_text.replace(
                phase2_limit, phase2_probe, 1
            )
        (temporary / "move_generator.h").write_text(move_generator_text)
        overflow_source = temporary / "overflow_probe.cpp"
        overflow_binary = temporary / "overflow_probe"
        overflow_source.write_text(OVERFLOW_PROBE)
        _compile(compiler, overflow_source, overflow_binary, [temporary, ROOT / "src"])
        overflow_result = json.loads(subprocess.check_output([overflow_binary], text=True))

    payload = {
        "schema_version": 1,
        "compiler": compiler,
        "copy_probe": copy_result,
        "overflow_probe": overflow_result,
        "scope": "reachable random corpus and one noncanonical editor boundary state",
    }
    rendered = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(rendered + "\n")
    print(rendered)


if __name__ == "__main__":
    main()
