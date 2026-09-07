"""Compare move ordering on a complete-information SPN, without a node cutoff."""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, find_legal_action_index_by_usi, spn_to_game


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("position", type=Path)
    parser.add_argument("--depth", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--required-action")
    parser.add_argument("--refutation", action="store_true")
    parser.add_argument("--candidates", action="store_true")
    parser.add_argument("--dfpn", action="store_true")
    parser.add_argument("--preferred-action-code", action="append", type=int, default=[])
    parser.add_argument(
        "--require-conclusion", action="store_true",
        help="fail unless mate or permanent no-mate is proven",
    )
    args = parser.parse_args()
    if args.refutation and args.required_action:
        parser.error("--required-action is an attacker constraint, not a counterstrategy root filter")
    game = spn_to_game(args.position.read_text(encoding="utf-8").strip())
    required = (
        game.legal_actions[find_legal_action_index_by_usi(game, args.required_action)].pack()
        if args.required_action else 2 ** 64 - 1
    )
    started = time.monotonic()
    result = cs.solve_reveal_verified_mate_cpp(
        game, attacker=1 - int(game.current_player) if args.refutation else int(game.current_player),
        depth=args.depth, max_nodes=0,
        time_limit_seconds=args.seconds, required_root_action=required,
        exhaustive_attacker_actions=not args.candidates, exact_reveal_search=True,
        use_route_ordering=not args.baseline, include_proof_dag=False,
        cooperative_reveals=args.refutation,
        use_dfpn=args.dfpn,
        preferred_attacker_actions=args.preferred_action_code,
    )
    legal = {a.pack(): a for a in game.legal_actions}
    line = result["line"]
    root_action = line[0]["action_code"] if line else None
    if result["proven"]:
        status = "permanent_no_mate" if args.refutation else "mate"
    elif result["unknown_reason"] or args.refutation or args.candidates:
        status = "unknown"
    else:
        status = "no_mate_within_depth"
    print(json.dumps({
        "ordering": "baseline" if args.baseline else "dual_route",
        "status": status,
        "depth": args.depth, "seconds": args.seconds,
        "wall_seconds": time.monotonic() - started,
        "root_action": action_to_usi(legal[root_action], game=game) if root_action in legal else None,
        "result": result,
    }, ensure_ascii=False), flush=True)
    if args.require_conclusion and status not in {"mate", "permanent_no_mate"}:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
