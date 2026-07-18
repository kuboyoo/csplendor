#!/usr/bin/env python3
"""Command-line orchestration for ``scripts.dfpn_mate_solver``."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Optional, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    api: Optional[Any] = None,
) -> int:
    """Run the DFPN CLI against an injected legacy API namespace."""
    if api is None:
        from scripts import dfpn_mate_solver as api

    cs = api.cs
    load_game_from_json = api.load_game_from_json
    load_game_from_usi_text = api.load_game_from_usi_text
    load_game_from_usi_file = api.load_game_from_usi_file
    apply_usi_moves = api.apply_usi_moves
    _parse_moves = api._parse_moves
    SolverOptions = api.SolverOptions
    solve_visible_only_winner = api.solve_visible_only_winner
    solve_reveal_verified_mate = api.solve_reveal_verified_mate
    solve_game_dfpn = api.solve_game_dfpn
    write_principal_line_kifu = api.write_principal_line_kifu
    write_mate_kifu = api.write_mate_kifu
    SearchResult = api.SearchResult
    SearchStats = api.SearchStats
    _DFPN_DEFAULT_PRUNING = api._DFPN_DEFAULT_PRUNING
    INVALID_INPUT = api.INVALID_INPUT
    MATE = api.MATE
    NO_MATE = api.NO_MATE
    PLAYER0_WIN = api.PLAYER0_WIN
    PLAYER1_WIN = api.PLAYER1_WIN
    DRAW = api.DRAW
    parser = argparse.ArgumentParser(
        description="Search guaranteed Splendor mate with depth-first proof-number search."
    )
    state_group = parser.add_mutually_exclusive_group()
    state_group.add_argument("--state-json", help="JSON file describing an arbitrary state")
    state_group.add_argument("--position", help="USI position command or raw SPN text")
    state_group.add_argument("--position-file", help="file containing a USI position command or raw SPN")
    parser.add_argument("--seed", type=int, default=0, help="initial game seed when state-json is omitted")
    parser.add_argument("--moves", action="append", default=[], help="USI move list, comma-separated or repeated")
    parser.add_argument("--attacker", type=int, default=0, choices=(0, 1))
    parser.add_argument("--max-depth", type=int, default=4, help="attacker turn depth for DFPN; ignored by --visible-only-winner and --reveal-verified")
    parser.add_argument("--node-limit", type=int)
    parser.add_argument("--time-limit", type=float, default=10.0)
    parser.add_argument(
        "--simple-payment",
        action="store_true",
        help="generate only canonical purchase payments that preserve gold when possible",
    )
    parser.add_argument(
        "--visible-only-winner",
        "--visible-only",
        dest="visible_only_winner",
        action="store_true",
        help="ignore hidden deck reveals and solve the visible-card game with full-response minimax; not a mate proof",
    )
    parser.add_argument(
        "--reveal-verified",
        action="store_true",
        help="find a visible-only candidate mate, then verify all defender responses and hidden reveal shapes",
    )
    parser.add_argument(
        "--reveal-proof-dag",
        action="store_true",
        help="with --reveal-verified, emit the proven strategy DAG after search",
    )
    parser.add_argument(
        "--proof-dag-node-limit",
        type=int,
        default=100000,
        help="maximum nodes emitted by --reveal-proof-dag; 0 disables the limit",
    )
    parser.add_argument(
        "--proof-dag-edge-limit",
        type=int,
        default=500000,
        help="maximum edges emitted by --reveal-proof-dag; 0 disables the limit",
    )
    parser.add_argument(
        "--proof-dag-format",
        choices=("compact", "v1", "both"),
        default="compact",
        help="strategy DAG encoding emitted by --reveal-proof-dag",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="worker processes for root-parallel DFPN search; 0 uses CPU count",
    )
    parser.add_argument("--allow-deck-reserve", action="store_true")
    parser.add_argument(
        "--no-threat-reveal-pruning",
        action="store_true",
        help="disable immediate-win threat based reveal collapsing",
    )
    parser.add_argument(
        "--no-lazy-reveal-pruning",
        action="store_true",
        help="disable delayed blank reveal refinement before concrete reveal branching",
    )
    parser.add_argument(
        "--no-attacker-dependency-pruning",
        action="store_true",
        help="disable lazy pruning of attacker moves outside the score dependency cone",
    )
    parser.add_argument(
        "--no-defender-relevance-pruning",
        action="store_true",
        help="disable lazy deferral of defender moves that do not affect the attacker's race plan",
    )
    parser.add_argument(
        "--no-equivalence-hash",
        action="store_true",
        help="disable threat-equivalence hashing and use exact state keys",
    )
    parser.add_argument(
        "--no-return-pattern-pruning",
        action="store_true",
        help="Disable representative pruning for equivalent payment/return patterns.",
    )
    parser.add_argument(
        "--no-upper-bound-pruning",
        action="store_true",
        help="Disable pruning by the attacker's optimistic score upper bound.",
    )
    parser.add_argument(
        "--no-immediate-terminal-pruning",
        action="store_true",
        help="Disable immediate win/defense terminal checks before expanding children.",
    )
    parser.add_argument(
        "--defender-threat-filter",
        action="store_true",
        help="Only expand defender replies that address immediate attacker threats.",
    )
    parser.add_argument(
        "--max-actions-per-node",
        type=int,
        default=0,
        help="Optional cap after move ordering and pruning; 0 means no cap.",
    )
    parser.add_argument(
        "--target-candidate-limit",
        type=int,
        default=int(_DFPN_DEFAULT_PRUNING["target_candidate_limit"]),
        help="Limit scored target cards for dependency pruning; 0 disables the limit.",
    )
    parser.add_argument(
        "--parallel-tt-limit",
        type=int,
        default=10000,
        help="max transposition-table entries per parallel worker; 0 disables worker memo",
    )
    parser.add_argument(
        "--parallel-start-method",
        choices=("spawn", "fork", "forkserver"),
        default="spawn",
        help="multiprocessing start method for parallel DFPN workers",
    )
    parser.add_argument("--progress", action="store_true", help="print periodic progress to stderr")
    parser.add_argument("--progress-interval", type=float, default=1.0, help="progress output interval in seconds")
    parser.add_argument("--no-memo", action="store_true")
    parser.add_argument("--no-proof", action="store_true")
    parser.add_argument(
        "--kifu-output",
        help="write one replayable principal line from a mate proof as Splendor KIFU",
    )
    parser.add_argument(
        "--kifu-dfpn",
        action="store_true",
        help="with --kifu-output, use the regular depth-limited DFPN proof instead of reveal-verified mate",
    )
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.reveal_proof_dag and not args.reveal_verified:
            raise ValueError("--reveal-proof-dag requires --reveal-verified")
        if args.reveal_proof_dag and args.no_proof:
            raise ValueError("--reveal-proof-dag cannot be combined with --no-proof")
        if args.kifu_output and args.no_proof:
            raise ValueError("--kifu-output cannot be combined with --no-proof")
        if args.kifu_dfpn and not args.kifu_output:
            raise ValueError("--kifu-dfpn requires --kifu-output")
        if args.kifu_dfpn and args.reveal_verified:
            raise ValueError("--kifu-dfpn cannot be combined with --reveal-verified")
        if args.kifu_output and not args.kifu_dfpn:
            args.reveal_verified = True
        if args.state_json:
            game = load_game_from_json(args.state_json)
        elif args.position:
            game = load_game_from_usi_text(args.position, seed=args.seed)
        elif args.position_file:
            game = load_game_from_usi_file(args.position_file, seed=args.seed)
        else:
            game = cs.Game(seed=args.seed)
        if args.simple_payment:
            game.simple_payment_mode = True
        apply_usi_moves(game, _parse_moves(args.moves))
        default_node_limit = 0 if args.visible_only_winner or args.reveal_verified else 200000
        options = SolverOptions(
            max_nodes=default_node_limit if args.node_limit is None else args.node_limit,
            time_limit=args.time_limit,
            include_proof=not args.no_proof,
            allow_deck_reserve=args.allow_deck_reserve,
            use_memo=not args.no_memo,
            jobs=args.jobs,
        )
        if args.visible_only_winner:
            result = solve_visible_only_winner(game, options=options)
            print(json.dumps(result.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
            return 0 if result.status in (PLAYER0_WIN, PLAYER1_WIN, DRAW) else 2
        if args.reveal_verified:
            result = solve_reveal_verified_mate(
                game,
                attacker=args.attacker,
                options=options,
                include_proof_dag=args.reveal_proof_dag,
                proof_dag_node_limit=args.proof_dag_node_limit,
                proof_dag_edge_limit=args.proof_dag_edge_limit,
                proof_dag_format=args.proof_dag_format,
            )
            if args.kifu_output:
                if result.status != MATE or result.proof_tree is None:
                    raise ValueError("--kifu-output requires a mate proof")
                line = result.proof_tree.get("line")
                if not isinstance(line, list):
                    raise ValueError("--kifu-output is not supported for this reveal-verified proof")
                write_principal_line_kifu(args.kifu_output, game, line, attacker=args.attacker)
            print(json.dumps(result.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
            return 0 if result.status == MATE else 2
        _DFPN_DEFAULT_PRUNING.update(
            {
                "lazy_reveal": not args.no_lazy_reveal_pruning,
                "attacker_dependency": not args.no_attacker_dependency_pruning,
                "defender_relevance": not args.no_defender_relevance_pruning,
                "return_pattern": not args.no_return_pattern_pruning,
                "upper_bound": not args.no_upper_bound_pruning,
                "immediate_terminal": not args.no_immediate_terminal_pruning,
                "defender_threat_filter": args.defender_threat_filter,
                "max_actions_per_node": max(0, int(args.max_actions_per_node)),
                "target_candidate_limit": max(0, int(args.target_candidate_limit)),
            }
        )
        result = solve_game_dfpn(
            game,
            attacker=args.attacker,
            max_depth=args.max_depth,
            options=options,
            use_lazy_reveal_pruning=not args.no_lazy_reveal_pruning,
            use_attacker_dependency_pruning=not args.no_attacker_dependency_pruning,
            use_defender_relevance_pruning=not args.no_defender_relevance_pruning,
            use_threat_reveal_pruning=not args.no_threat_reveal_pruning,
            use_equivalence_hash=not args.no_equivalence_hash,
            use_return_pattern_pruning=not args.no_return_pattern_pruning,
            use_upper_bound_pruning=not args.no_upper_bound_pruning,
            use_immediate_terminal_pruning=not args.no_immediate_terminal_pruning,
            use_defender_threat_filter=args.defender_threat_filter,
            max_actions_per_node=max(0, int(args.max_actions_per_node)),
            target_candidate_limit=max(0, int(args.target_candidate_limit)),
            parallel_tt_limit=args.parallel_tt_limit,
            show_progress=args.progress,
            progress_interval=args.progress_interval,
            parallel_start_method=args.parallel_start_method,
        )
        if args.kifu_output:
            if result.status != MATE or result.proof_tree is None:
                raise ValueError("--kifu-output requires a mate proof")
            write_mate_kifu(args.kifu_output, game, result.proof_tree, attacker=args.attacker)
        print(json.dumps(result.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
        return 0 if result.status in (MATE, NO_MATE) else 2
    except Exception as exc:
        error = SearchResult(
            status=INVALID_INPUT,
            depth=None,
            proof_tree=None,
            refutation={"error": str(exc)},
            stats=SearchStats(),
        )
        print(json.dumps(error.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
        return 1



if __name__ == "__main__":
    raise SystemExit(main())
