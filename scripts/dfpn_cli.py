#!/usr/bin/env python3
"""Command line of ``scripts.dfpn_mate_solver`` (reveal-verified mate search)."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    api: Optional[Any] = None,
) -> int:
    """Run the CLI against an injected API namespace (scripts.dfpn_mate_solver)."""
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
    write_principal_line_kifu = api.write_principal_line_kifu
    SearchResult = api.SearchResult
    SearchStats = api.SearchStats
    INVALID_INPUT = api.INVALID_INPUT
    MATE = api.MATE
    PLAYER0_WIN = api.PLAYER0_WIN
    PLAYER1_WIN = api.PLAYER1_WIN
    DRAW = api.DRAW
    parser = argparse.ArgumentParser(
        description=(
            "Search a guaranteed Splendor mate verified over every defender reply "
            "and hidden reveal (native solver; doc/mate_usage.md)."
        )
    )
    state_group = parser.add_mutually_exclusive_group()
    state_group.add_argument("--state-json", help="JSON file describing an arbitrary state")
    state_group.add_argument("--position", help="USI position command or raw SPN text")
    state_group.add_argument("--position-file", help="file containing a USI position command or raw SPN")
    parser.add_argument("--seed", type=int, default=0, help="initial game seed when state-json is omitted")
    parser.add_argument("--moves", action="append", default=[], help="USI move list, comma-separated or repeated")
    parser.add_argument("--attacker", type=int, default=0, choices=(0, 1))
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
        "--reveal-depth-range",
        nargs=2,
        type=int,
        metavar=("MIN", "MAX"),
        help=(
            "test every attacker depth from MIN through MAX; "
            "stop on mate, unknown, or MAX"
        ),
    )
    parser.add_argument(
        "--required-root-action",
        help="USI root action to hold fixed during --reveal-depth-range",
    )
    parser.add_argument(
        "--reveal-anytime",
        action="store_true",
        help=(
            "with --reveal-depth-range, advance after inconclusive depths and "
            "return only positive mate proofs; intended for live AI clocks"
        ),
    )
    parser.add_argument(
        "--reveal-proof-dag",
        action="store_true",
        help="emit the proven strategy DAG after search",
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
        help=(
            "parallel workers for exact reveal root branches; "
            "0 uses CPU count"
        ),
    )
    parser.add_argument("--allow-deck-reserve", action="store_true")
    parser.add_argument("--no-memo", action="store_true")
    parser.add_argument("--no-proof", action="store_true")
    parser.add_argument(
        "--kifu-output",
        help="write one replayable principal line from a mate proof as Splendor KIFU",
    )
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.visible_only_winner and (
            args.reveal_proof_dag or args.reveal_depth_range or args.kifu_output
        ):
            raise ValueError(
                "--visible-only-winner cannot be combined with mate proof options"
            )
        if args.required_root_action and not args.reveal_depth_range:
            raise ValueError("--required-root-action requires --reveal-depth-range")
        if args.reveal_anytime and not args.reveal_depth_range:
            raise ValueError("--reveal-anytime requires --reveal-depth-range")
        if args.reveal_anytime and args.reveal_proof_dag:
            raise ValueError("--reveal-anytime does not build a proof DAG")
        if args.reveal_depth_range and args.kifu_output:
            raise ValueError("--kifu-output is not supported with --reveal-depth-range")
        if args.reveal_proof_dag and args.no_proof:
            raise ValueError("--reveal-proof-dag cannot be combined with --no-proof")
        if args.kifu_output and args.no_proof:
            raise ValueError("--kifu-output cannot be combined with --no-proof")
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
        options = SolverOptions(
            max_nodes=0 if args.node_limit is None else args.node_limit,
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
        if args.reveal_depth_range:
            from csplendor.api.usi_kifu import find_legal_action_index_by_usi

            min_depth, max_depth = (int(value) for value in args.reveal_depth_range)
            required_action = None
            if args.required_root_action:
                action_index = find_legal_action_index_by_usi(
                    game, args.required_root_action
                )
                required_action = int(game.legal_actions[action_index].pack())
            search_function = (
                cs.search_reveal_verified_mate_anytime
                if args.reveal_anytime
                else cs.search_reveal_verified_mate_depths
            )
            depth_search = search_function(
                game,
                attacker=args.attacker,
                min_depth=min_depth,
                max_depth=max_depth,
                max_nodes=options.max_nodes,
                time_limit_seconds=options.time_limit,
                required_root_action=required_action,
                jobs=args.jobs,
                **(
                    {}
                    if args.reveal_anytime
                    else {
                        "include_proof_dag": args.reveal_proof_dag,
                        "proof_dag_node_limit": args.proof_dag_node_limit,
                        "proof_dag_edge_limit": args.proof_dag_edge_limit,
                        "proof_dag_format": (
                            "compact"
                            if args.proof_dag_format == "both"
                            else args.proof_dag_format
                        ),
                    }
                ),
            )
            print(
                json.dumps(
                    depth_search,
                    indent=2 if args.pretty else None,
                    sort_keys=True,
                )
            )
            return 0 if depth_search["status"] in {
                "mate",
                "no_mate_within_max_depth",
                "permanent_no_mate",
            } else 2
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
