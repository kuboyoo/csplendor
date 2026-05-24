#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, find_legal_action_index_by_usi


MATE = "Mate"
NO_MATE = "NoMate"
UNKNOWN = "Unknown"
INVALID_INPUT = "InvalidInput"


@dataclass(frozen=True)
class SolverOptions:
    max_nodes: int = 200000
    time_limit: float = 10.0
    include_proof: bool = True
    allow_deck_reserve: bool = False
    use_memo: bool = True


@dataclass
class SearchStats:
    nodes: int = 0
    memo_hits: int = 0
    terminal_nodes: int = 0
    reveal_branches: int = 0
    legal_moves: int = 0
    elapsed_ms: float = 0.0
    max_depth_reached: int = 0
    unknown_reason: Optional[str] = None


@dataclass(frozen=True)
class SolverState:
    game: cs.Game
    unseen_by_level: Tuple[frozenset, frozenset, frozenset]

    @staticmethod
    def from_game(game: cs.Game) -> "SolverState":
        return SolverState(
            game=game.clone_light(),
            unseen_by_level=tuple(
                frozenset(int(card_id) for card_id in deck)
                for deck in game.board.decks
            ),
        )


@dataclass
class SearchResult:
    status: str
    depth: Optional[int]
    proof_tree: Optional[Dict[str, Any]]
    refutation: Optional[Dict[str, Any]]
    stats: SearchStats

    def to_dict(self) -> Dict[str, Any]:
        return {
            "status": self.status,
            "depth": self.depth,
            "proof_tree": self.proof_tree,
            "refutation": self.refutation,
            "stats": asdict(self.stats),
        }


@dataclass
class _NodeResult:
    winning: bool
    proof: Optional[Dict[str, Any]] = None
    refutation: Optional[Dict[str, Any]] = None


@dataclass(frozen=True)
class _Outcome:
    reveal_level: Optional[int]
    reveal_card: Optional[int]
    children: Tuple[SolverState, ...]


class SearchLimitExceeded(Exception):
    pass


class MateSolver:
    def __init__(self, attacker: int, max_depth: int, options: Optional[SolverOptions] = None):
        if attacker not in (0, 1):
            raise ValueError("attacker must be 0 or 1")
        if max_depth < 0:
            raise ValueError("max_depth must be non-negative")
        self.attacker = attacker
        self.max_depth = max_depth
        self.options = options or SolverOptions()
        self.stats = SearchStats()
        self._memo: Dict[Tuple[Any, int], bool] = {}
        self._start_time = 0.0

    def solve(self, state: SolverState) -> SearchResult:
        self.stats = SearchStats()
        self._memo = {}
        self._start_time = time.monotonic()

        try:
            result = self._solve(state, self.max_depth)
            status = MATE if result.winning else NO_MATE
            depth = self.max_depth if result.winning else None
            return SearchResult(status, depth, result.proof, result.refutation, self._finish_stats())
        except SearchLimitExceeded as exc:
            self.stats.unknown_reason = str(exc)
            return SearchResult(UNKNOWN, None, None, None, self._finish_stats())

    def _finish_stats(self) -> SearchStats:
        self.stats.elapsed_ms = (time.monotonic() - self._start_time) * 1000.0
        return self.stats

    def _solve(self, state: SolverState, depth: int) -> _NodeResult:
        self._check_limits()
        self.stats.nodes += 1
        self.stats.max_depth_reached = max(self.stats.max_depth_reached, self.max_depth - depth)

        game = state.game
        board = game.board
        if game.is_game_over():
            self.stats.terminal_nodes += 1
            winning = int(game.winner) == self.attacker
            node = self._state_summary(state, depth)
            node["terminal_winner"] = int(game.winner)
            if winning:
                return _NodeResult(True, proof=node if self.options.include_proof else None)
            return _NodeResult(False, refutation=node if self.options.include_proof else None)

        key = (self._canonical_key(state), depth)
        if self.options.use_memo and key in self._memo:
            self.stats.memo_hits += 1
            winning = self._memo[key]
            cached = {"cached": True, "depth": depth} if self.options.include_proof else None
            return _NodeResult(winning, proof=cached if winning else None, refutation=None if winning else cached)

        if bool(board.waiting_noble):
            result = self._solve_waiting_noble(state, depth)
        elif int(board.current_player) == self.attacker:
            if depth <= 0:
                node = self._state_summary(state, depth)
                node["reason"] = "attacker_depth_exhausted"
                result = _NodeResult(False, refutation=node if self.options.include_proof else None)
            else:
                result = self._solve_attacker_turn(state, depth)
        else:
            result = self._solve_defender_turn(state, depth)

        if self.options.use_memo:
            self._memo[key] = result.winning
        return result

    def _solve_waiting_noble(self, state: SolverState, depth: int) -> _NodeResult:
        choices = [
            action for action in state.game.legal_actions
            if int(action.type) == int(cs.ActionType.VISIT_NOBLE)
        ]
        self.stats.legal_moves += len(choices)
        attacker_choice = int(state.game.board.current_player) == self.attacker

        if attacker_choice:
            failures: List[Dict[str, Any]] = []
            for action in self._ordered_actions(choices):
                child = self._apply_noble_choice(state, action)
                child_result = self._solve(child, depth)
                if child_result.winning:
                    proof = self._choice_node(state, depth, action, None, [child_result.proof])
                    return _NodeResult(True, proof=proof if self.options.include_proof else None)
                if self.options.include_proof:
                    failures.append(self._failure_node(action, None, child_result.refutation))
            node = self._state_summary(state, depth)
            node["failed_noble_choices"] = failures
            return _NodeResult(False, refutation=node if self.options.include_proof else None)

        proof_choices: List[Dict[str, Any]] = []
        for action in self._ordered_actions(choices):
            child = self._apply_noble_choice(state, action)
            child_result = self._solve(child, depth)
            if not child_result.winning:
                refutation = self._failure_node(action, None, child_result.refutation)
                return _NodeResult(False, refutation=refutation if self.options.include_proof else None)
            if self.options.include_proof:
                proof_choices.append(self._choice_node(state, depth, action, None, [child_result.proof]))

        node = self._state_summary(state, depth)
        node["all_defender_noble_choices"] = proof_choices
        return _NodeResult(True, proof=node if self.options.include_proof else None)

    def _solve_attacker_turn(self, state: SolverState, depth: int) -> _NodeResult:
        actions = self._legal_actions(state)
        failures: List[Dict[str, Any]] = []
        for action in self._ordered_actions(actions):
            outcomes = self._transition_outcomes(state, action)
            if not outcomes:
                continue

            proof_branches: List[Dict[str, Any]] = []
            failed = None
            for outcome in outcomes:
                child_win = None
                child_proof = None
                child_failures: List[Dict[str, Any]] = []
                for child in outcome.children:
                    child_result = self._solve(child, depth - 1)
                    if child_result.winning:
                        child_win = child_result
                        child_proof = child_result.proof
                        break
                    if self.options.include_proof and child_result.refutation is not None:
                        child_failures.append(child_result.refutation)
                if child_win is None:
                    failed = self._failure_node(action, outcome, {"children": child_failures})
                    break
                if self.options.include_proof:
                    proof_branches.append(self._outcome_node(action, outcome, child_proof))

            if failed is None:
                proof = self._choice_node(state, depth, action, outcomes, proof_branches)
                return _NodeResult(True, proof=proof if self.options.include_proof else None)
            if self.options.include_proof:
                failures.append(failed)

        node = self._state_summary(state, depth)
        node["failed_attacker_moves"] = failures
        return _NodeResult(False, refutation=node if self.options.include_proof else None)

    def _solve_defender_turn(self, state: SolverState, depth: int) -> _NodeResult:
        actions = self._legal_actions(state)
        proof_moves: List[Dict[str, Any]] = []
        for action in self._ordered_actions(actions):
            outcomes = self._transition_outcomes(state, action)
            if not outcomes:
                refutation = self._failure_node(action, None, {"reason": "no_transition"})
                return _NodeResult(False, refutation=refutation if self.options.include_proof else None)

            proof_outcomes: List[Dict[str, Any]] = []
            for outcome in outcomes:
                for child in outcome.children:
                    child_result = self._solve(child, depth)
                    if not child_result.winning:
                        refutation = self._outcome_node(action, outcome, child_result.refutation)
                        return _NodeResult(False, refutation=refutation if self.options.include_proof else None)
                    if self.options.include_proof:
                        proof_outcomes.append(self._outcome_node(action, outcome, child_result.proof))

            if self.options.include_proof:
                proof_moves.append(self._choice_node(state, depth, action, outcomes, proof_outcomes))

        node = self._state_summary(state, depth)
        node["all_defender_moves"] = proof_moves
        return _NodeResult(True, proof=node if self.options.include_proof else None)

    def _check_limits(self) -> None:
        if self.options.max_nodes and self.stats.nodes >= self.options.max_nodes:
            raise SearchLimitExceeded("node limit exceeded")
        if self.options.time_limit and (time.monotonic() - self._start_time) >= self.options.time_limit:
            raise SearchLimitExceeded("time limit exceeded")

    def _legal_actions(self, state: SolverState) -> List[cs.Action]:
        actions = []
        for action in state.game.legal_actions:
            action_type = int(action.type)
            if action_type == int(cs.ActionType.VISIT_NOBLE):
                continue
            if action_type == int(cs.ActionType.RESERVE_DECK) and not self.options.allow_deck_reserve:
                continue
            actions.append(action)
        self.stats.legal_moves += len(actions)
        return actions

    def _ordered_actions(self, actions: Iterable[cs.Action]) -> List[cs.Action]:
        return sorted(actions, key=self._action_order_key)

    def _action_order_key(self, action: cs.Action) -> Tuple[int, int, int]:
        action_type = int(action.type)
        points = 0
        if action_type in (int(cs.ActionType.PURCHASE), int(cs.ActionType.RESERVE_VISIBLE)):
            try:
                points = int(cs.get_card(int(action.card_id)).points)
            except Exception:
                points = 0
        rank = {
            int(cs.ActionType.PURCHASE): 0,
            int(cs.ActionType.RESERVE_VISIBLE): 1,
            int(cs.ActionType.TAKE_SAME): 2,
            int(cs.ActionType.TAKE_DIFFERENT): 3,
            int(cs.ActionType.RESERVE_DECK): 4,
            int(cs.ActionType.VISIT_NOBLE): 5,
        }.get(action_type, 9)
        return (rank, -points, int(action.pack()))

    def _transition_outcomes(self, state: SolverState, action: cs.Action) -> List[_Outcome]:
        action_type = int(action.type)
        level: Optional[int] = None

        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            level = int(cs.get_card(int(action.card_id)).level) - 1
        elif action_type == int(cs.ActionType.PURCHASE) and not bool(action.from_reserved):
            level = int(cs.get_card(int(action.card_id)).level) - 1
        elif action_type == int(cs.ActionType.RESERVE_DECK):
            level = int(action.deck_level)

        if level is None:
            return [self._apply_with_reveal(state, action, None, None)]

        candidates = sorted(int(card_id) for card_id in state.unseen_by_level[level])
        if not candidates:
            return [self._apply_with_reveal(state, action, level, None)]

        outcomes = [
            self._apply_with_reveal(state, action, level, card_id)
            for card_id in self._ordered_reveals(candidates)
        ]
        self.stats.reveal_branches += len(outcomes)
        return outcomes

    def _ordered_reveals(self, card_ids: Sequence[int]) -> List[int]:
        return sorted(
            card_ids,
            key=lambda card_id: (-int(cs.get_card(card_id).points), int(cs.get_card(card_id).bonus), card_id),
        )

    def _apply_with_reveal(
        self,
        state: SolverState,
        action: cs.Action,
        level: Optional[int],
        card_id: Optional[int],
    ) -> _Outcome:
        game = state.game.clone_light()
        game.board.decks = self._deck_order_for(state, level, card_id)

        if not game.apply(action, False):
            raise RuntimeError("engine rejected a legal action during mate transition")

        next_unseen = [set(cards) for cards in state.unseen_by_level]
        if level is not None and card_id is not None:
            next_unseen[level].discard(card_id)

        raw_child = SolverState(
            game=game,
            unseen_by_level=tuple(frozenset(cards) for cards in next_unseen),
        )
        children = self._finalize_noble_choices(raw_child)
        return _Outcome(level, card_id, children)

    def _deck_order_for(
        self,
        state: SolverState,
        reveal_level: Optional[int],
        reveal_card: Optional[int],
    ) -> List[List[int]]:
        decks: List[List[int]] = []
        for level, unseen in enumerate(state.unseen_by_level):
            cards = sorted(int(card_id) for card_id in unseen)
            if level == reveal_level:
                if reveal_card is None:
                    cards = []
                else:
                    cards = [card_id for card_id in cards if card_id != reveal_card]
                    cards.append(int(reveal_card))
            decks.append(cards)
        return decks

    def _finalize_noble_choices(self, state: SolverState) -> Tuple[SolverState, ...]:
        if not bool(state.game.board.waiting_noble):
            return (state,)

        children: List[SolverState] = []
        for action in self._ordered_actions(state.game.legal_actions):
            if int(action.type) != int(cs.ActionType.VISIT_NOBLE):
                continue
            children.append(self._apply_noble_choice(state, action))
        return tuple(self._dedupe_states(children))

    def _apply_noble_choice(self, state: SolverState, action: cs.Action) -> SolverState:
        game = state.game.clone_light()
        if not game.apply(action, False):
            raise RuntimeError("engine rejected a legal noble action during mate transition")
        return SolverState(game=game, unseen_by_level=state.unseen_by_level)

    def _dedupe_states(self, states: Sequence[SolverState]) -> List[SolverState]:
        seen = set()
        out: List[SolverState] = []
        for state in states:
            key = self._canonical_key(state)
            if key not in seen:
                seen.add(key)
                out.append(state)
        return out

    def _canonical_key(self, state: SolverState) -> Tuple[Any, ...]:
        board = state.game.board
        players = []
        for player_idx in range(2):
            player = board.get_player(player_idx)
            players.append(
                (
                    int(player.points),
                    tuple(int(v) for v in player.gems),
                    tuple(int(v) for v in player.bonuses),
                    tuple(int(v) for v in player.reserved),
                    tuple(bool(v) for v in player.reserved_is_hidden),
                    int(player.reserved_count),
                    int(player.purchased_count),
                    tuple(sorted(int(v) for v in player.purchased_cards)),
                    tuple(sorted(int(v) for v in player.acquired_nobles)),
                )
            )

        return (
            int(board.turn),
            int(board.current_player),
            bool(board.final_round),
            bool(board.waiting_noble),
            int(board.winner),
            tuple(int(v) for v in board.bank),
            tuple(tuple(int(card_id) for card_id in row) for row in board.visible),
            tuple(sorted(int(noble_id) for noble_id in board.nobles)),
            tuple(players),
            tuple(tuple(sorted(int(card_id) for card_id in level)) for level in state.unseen_by_level),
            bool(state.game.simple_payment_mode),
            bool(state.game.blank_refill_mode),
        )

    def _state_summary(self, state: SolverState, depth: int) -> Dict[str, Any]:
        board = state.game.board
        return {
            "depth_remaining": depth,
            "current_player": int(board.current_player),
            "turn": int(board.turn),
            "scores": [int(v) for v in state.game.scores],
            "winner": int(board.winner),
            "final_round": bool(board.final_round),
            "waiting_noble": bool(board.waiting_noble),
            "unseen_counts": [len(level) for level in state.unseen_by_level],
        }

    def _action_summary(self, action: cs.Action, game: Optional[cs.Game] = None) -> Dict[str, Any]:
        try:
            usi = action_to_usi(action, game=game)
        except Exception:
            usi = None
        return {
            "type": int(action.type),
            "repr": repr(action),
            "pack": int(action.pack()),
            "usi": usi,
        }

    def _choice_node(
        self,
        state: SolverState,
        depth: int,
        action: cs.Action,
        outcomes: Optional[Sequence[_Outcome]],
        branches: Sequence[Optional[Dict[str, Any]]],
    ) -> Dict[str, Any]:
        node = self._state_summary(state, depth)
        node["action"] = self._action_summary(action, state.game)
        if outcomes is not None:
            node["outcome_count"] = len(outcomes)
        node["branches"] = [branch for branch in branches if branch is not None]
        return node

    def _outcome_node(
        self,
        action: cs.Action,
        outcome: _Outcome,
        child: Optional[Dict[str, Any]],
    ) -> Dict[str, Any]:
        return {
            "action": self._action_summary(action),
            "reveal_level": None if outcome.reveal_level is None else outcome.reveal_level + 1,
            "reveal_card": outcome.reveal_card,
            "noble_choice_count": len(outcome.children),
            "child": child,
        }

    def _failure_node(
        self,
        action: cs.Action,
        outcome: Optional[_Outcome],
        child: Optional[Dict[str, Any]],
    ) -> Dict[str, Any]:
        if outcome is None:
            return {"action": self._action_summary(action), "child": child}
        return self._outcome_node(action, outcome, child)


def solve_game(
    game: cs.Game,
    attacker: int,
    max_depth: int,
    options: Optional[SolverOptions] = None,
) -> SearchResult:
    solver = MateSolver(attacker=attacker, max_depth=max_depth, options=options)
    return solver.solve(SolverState.from_game(game))


def load_game_from_json(path: str) -> cs.Game:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)

    game = cs.Game(seed=int(data.get("seed", 0)))
    board = game.board

    if "simple_payment_mode" in data:
        game.simple_payment_mode = bool(data["simple_payment_mode"])
    if "blank_refill_mode" in data:
        game.blank_refill_mode = bool(data["blank_refill_mode"])

    if "bank" in data:
        board.bank = _fixed_int_list(data["bank"], 6, "bank")
    if "visible" in data:
        board.visible = data["visible"]
    if "decks" in data:
        board.decks = data["decks"]
    if "nobles" in data:
        board.nobles = data["nobles"]
    if "current_player" in data:
        board.current_player = int(data["current_player"])
    if "turn" in data:
        board.turn = int(data["turn"])
    if "final_round" in data:
        board.final_round = bool(data["final_round"])
    if "waiting_noble" in data:
        board.waiting_noble = bool(data["waiting_noble"])
    if "winner" in data:
        board.winner = int(data["winner"])

    for idx, player_data in enumerate(data.get("players", [])):
        if idx >= 2:
            break
        player = board.get_player(idx)
        if "gems" in player_data:
            player.gems = _fixed_int_list(player_data["gems"], 6, "player.gems")
        if "bonuses" in player_data:
            player.bonuses = _fixed_int_list(player_data["bonuses"], 5, "player.bonuses")
        if "points" in player_data:
            player.points = int(player_data["points"])
        if "reserved" in player_data:
            player.reserved = _fixed_int_list(player_data["reserved"], 3, "player.reserved")
        if "reserved_is_hidden" in player_data:
            vals = player_data["reserved_is_hidden"]
            if len(vals) != 3:
                raise ValueError("player.reserved_is_hidden must have 3 values")
            player.reserved_is_hidden = [bool(v) for v in vals]
        if "reserved_count" in player_data:
            player.reserved_count = int(player_data["reserved_count"])
        if "purchased_count" in player_data:
            player.purchased_count = int(player_data["purchased_count"])
        if "purchased_cards" in player_data:
            player.purchased_cards = [int(v) for v in player_data["purchased_cards"]]
        if "acquired_nobles" in player_data:
            player.acquired_nobles = [int(v) for v in player_data["acquired_nobles"]]
        board.set_player(idx, player)

    return game


def apply_usi_moves(game: cs.Game, moves: Sequence[str]) -> None:
    for move in moves:
        move = move.strip()
        if not move:
            continue
        index = find_legal_action_index_by_usi(game, move)
        if index < 0:
            raise ValueError(f"no legal action matches move: {move}")
        action = game.legal_actions[index]
        if not game.apply(action, False):
            raise RuntimeError(f"engine rejected move: {move}")


def _fixed_int_list(values: Sequence[Any], length: int, name: str) -> List[int]:
    if len(values) != length:
        raise ValueError(f"{name} must have {length} values")
    return [int(v) for v in values]


def _parse_moves(values: Sequence[str]) -> List[str]:
    moves: List[str] = []
    for value in values:
        moves.extend(part.strip() for part in value.split(",") if part.strip())
    return moves


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Search guaranteed Splendor mate with universal reveal branching."
    )
    parser.add_argument("--state-json", help="JSON file describing an arbitrary state")
    parser.add_argument("--seed", type=int, default=0, help="initial game seed when state-json is omitted")
    parser.add_argument("--moves", action="append", default=[], help="USI move list, comma-separated or repeated")
    parser.add_argument("--attacker", type=int, default=0, choices=(0, 1))
    parser.add_argument("--max-depth", type=int, required=True)
    parser.add_argument("--node-limit", type=int, default=200000)
    parser.add_argument("--time-limit", type=float, default=10.0)
    parser.add_argument("--allow-deck-reserve", action="store_true")
    parser.add_argument("--no-proof", action="store_true")
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args(argv)

    try:
        game = load_game_from_json(args.state_json) if args.state_json else cs.Game(seed=args.seed)
        apply_usi_moves(game, _parse_moves(args.moves))
        options = SolverOptions(
            max_nodes=args.node_limit,
            time_limit=args.time_limit,
            include_proof=not args.no_proof,
            allow_deck_reserve=args.allow_deck_reserve,
        )
        result = solve_game(game, attacker=args.attacker, max_depth=args.max_depth, options=options)
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
