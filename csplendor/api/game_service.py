"""Framework-neutral game-session and KIFU-record application service."""

from __future__ import annotations

import uuid
from typing import Any, Callable, Dict, List, Optional, Tuple

from .. import Action, Game
from .application_errors import InvalidRequest, ResourceNotFound
from .kifu_codec import build_kifu_text, now_iso
from .spn_codec import game_to_spn
from .stores import KifuStore, SessionStore
from .usi_resolver import find_legal_action_index_by_usi
from .usi_serializer import action_to_usi

Record = Dict[str, Any]


class GameSessionService:
    def __init__(
        self,
        sessions: SessionStore[Game],
        records: KifuStore[Record],
        *,
        id_factory: Callable[[], str] = lambda: str(uuid.uuid4()),
    ) -> None:
        self.sessions = sessions
        self.records = records
        self._id_factory = id_factory

    def require_game(self, session_id: str) -> Game:
        game = self.sessions.get(session_id)
        if game is None:
            raise ResourceNotFound("Session not found")
        return game

    def require_record(self, session_id: str) -> Record:
        record = self.records.get(session_id)
        if record is None:
            raise ResourceNotFound("KIFU record not found for session")
        return record

    def create_game(
        self,
        *,
        seed: int,
        simple_payment_mode: bool,
        player0_name: str,
        player1_name: str,
    ) -> str:
        session_id = self._id_factory()
        game = Game(seed=seed)
        game.simple_payment_mode = simple_payment_mode
        self.sessions[session_id] = game
        self.set_session_record(
            session_id,
            seed=seed,
            simple_payment_mode=simple_payment_mode,
            initial_spn=game_to_spn(game),
            player0=player0_name,
            player1=player1_name,
        )
        return session_id

    def session_record_defaults(self, session_id: str) -> Record:
        return {
            "session_id": session_id,
            "meta": {
                "Format": "Splendor KIFU v1.0",
                "Players": "2",
                "Player0": "Player0",
                "Player1": "Player1",
                "Date": now_iso(),
            },
            "seed": 0,
            "simple_payment_mode": True,
            "initial_spn": "",
            "moves": [],
        }

    def set_session_record(
        self,
        session_id: str,
        *,
        seed: int,
        simple_payment_mode: bool,
        initial_spn: str,
        player0: str = "Player0",
        player1: str = "Player1",
    ) -> None:
        record = self.session_record_defaults(session_id)
        record["seed"] = int(seed)
        record["simple_payment_mode"] = bool(simple_payment_mode)
        record["initial_spn"] = initial_spn
        record["meta"]["Player0"] = player0
        record["meta"]["Player1"] = player1
        record["meta"]["Seed"] = str(int(seed))
        record["meta"]["SimplePaymentMode"] = (
            "1" if simple_payment_mode else "0"
        )
        self.records[session_id] = record

    def update_meta(self, session_id: str, values: Dict[str, Optional[str]]) -> None:
        self.require_game(session_id)
        meta = self.require_record(session_id)["meta"]
        for key, value in values.items():
            if value is not None:
                meta[key] = value

    def append_move(
        self,
        session_id: str,
        game: Game,
        action: Action,
        *,
        time_ms: Optional[int] = None,
        comment: Optional[str] = None,
    ) -> None:
        record = self.records.get(session_id)
        if record is None:
            return
        record["moves"].append(
            {
                "turn": int(game.board.turn),
                "player": int(game.board.current_player),
                "usi": action_to_usi(action, game=game),
                "time_ms": int(time_ms) if time_ms is not None else None,
                "comment": comment if comment else None,
            }
        )

    def apply_action(
        self,
        session_id: str,
        action_index: int,
        *,
        time_ms: Optional[int] = None,
        comment: Optional[str] = None,
    ) -> Game:
        game = self.require_game(session_id)
        legal_actions = game.legal_actions
        if action_index < 0 or action_index >= len(legal_actions):
            raise InvalidRequest("Invalid action index")
        action = legal_actions[action_index]
        self.append_move(
            session_id, game, action, time_ms=time_ms, comment=comment
        )
        game.apply(action)
        return game

    def apply_usi(
        self,
        session_id: str,
        usi_move: str,
        *,
        time_ms: Optional[int] = None,
        comment: Optional[str] = None,
    ) -> Tuple[int, str, Game]:
        game = self.require_game(session_id)
        try:
            action_index = find_legal_action_index_by_usi(game, usi_move)
        except ValueError as error:
            raise InvalidRequest(str(error)) from error
        if action_index < 0:
            raise InvalidRequest("pass is not legal in current state")
        action = game.legal_actions[action_index]
        canonical_usi = action_to_usi(action, game=game)
        self.append_move(
            session_id, game, action, time_ms=time_ms, comment=comment
        )
        game.apply(action)
        return action_index, canonical_usi, game

    def undo(self, session_id: str) -> Game:
        game = self.require_game(session_id)
        game.undo()
        record = self.records.get(session_id)
        if record is not None and record.get("moves"):
            record["moves"] = record["moves"][:-1]
        return game

    @staticmethod
    def result_from_game(game: Game) -> str:
        winner = int(game.board.winner)
        if winner == -2:
            return "DRAW"
        if winner >= 0:
            return f"P{winner}_WIN"
        return "ONGOING"

    @staticmethod
    def scores_from_game(game: Game) -> Optional[List[int]]:
        try:
            return [int(value) for value in game.scores()]
        except Exception:
            return None

    def build_kifu_text(
        self,
        session_id: str,
        override: Optional[Dict[str, Optional[str]]] = None,
    ) -> str:
        game = self.require_game(session_id)
        record = self.require_record(session_id)
        headers = dict(record.get("meta", {}))
        if override:
            for key, value in override.items():
                if value is not None:
                    headers[key] = value
        if "Date" not in headers:
            headers["Date"] = now_iso()
        if record.get("initial_spn"):
            headers["InitialSPN"] = str(record["initial_spn"])
        return build_kifu_text(
            headers=headers,
            position="startpos 2",
            moves=record.get("moves", []),
            result=self.result_from_game(game),
            final_scores=self.scores_from_game(game),
            total_turns=int(game.board.turn),
        )
