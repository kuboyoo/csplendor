"""Application service for KIFU persistence, replay and branching."""

from __future__ import annotations

import glob
import os
import re
import time
import uuid
from typing import Any, Callable, Dict, List, Optional

from .. import Game
from .application_errors import InvalidRequest, ResourceNotFound
from .game_service import GameSessionService
from .kifu_codec import parse_kifu_text
from .spn_codec import game_to_spn
from .stores import KifuStore
from .usi_resolver import find_legal_action_index_by_usi
from .usi_serializer import action_to_usi


class KifuApplicationService:
    def __init__(
        self,
        games: GameSessionService,
        replay_sessions: KifuStore[Dict[str, Any]],
        *,
        kifu_dir: Callable[[], str],
        state_serializer: Callable[[Game], Any],
        id_factory: Callable[[], str] = lambda: str(uuid.uuid4()),
    ) -> None:
        self.games = games
        self.replay_sessions = replay_sessions
        self._kifu_dir = kifu_dir
        self._state_serializer = state_serializer
        self._id_factory = id_factory

    @staticmethod
    def safe_filename(name: str) -> str:
        filename = re.sub(r"[^0-9A-Za-z._-]+", "_", name.strip())
        if not filename:
            filename = "game"
        if not filename.endswith(".kifu"):
            filename += ".kifu"
        return filename

    def export_session(
        self,
        session_id: str,
        override: Optional[Dict[str, Optional[str]]] = None,
    ) -> Dict[str, Any]:
        text = self.games.build_kifu_text(session_id, override)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = self.safe_filename(
            f"game_{session_id[:8]}_{timestamp}"
        )
        output_path = os.path.join(self._kifu_dir(), filename)
        with open(output_path, "w", encoding="utf-8") as stream:
            stream.write(text)
        return {"path": output_path, "filename": filename, "saved": True}

    def save_text(self, filename: str, text: str) -> Dict[str, Any]:
        safe_name = self.safe_filename(filename)
        output_path = os.path.join(self._kifu_dir(), safe_name)
        with open(output_path, "w", encoding="utf-8") as stream:
            stream.write(text)
        return {"path": output_path, "filename": safe_name, "saved": True}

    def list_files(self) -> List[Dict[str, Any]]:
        files: List[Dict[str, Any]] = []
        for path in sorted(glob.glob(os.path.join(self._kifu_dir(), "*.kifu"))):
            file_stat = os.stat(path)
            files.append(
                {
                    "filename": os.path.basename(path),
                    "path": path,
                    "size": int(file_stat.st_size),
                    "mtime": int(file_stat.st_mtime),
                }
            )
        return files

    def build_replay(self, text: str) -> Dict[str, Any]:
        parsed = parse_kifu_text(text)
        headers = parsed.get("headers", {})
        position = str(parsed.get("position", "")).strip()
        moves = parsed.get("moves", [])
        if str(headers.get("Players", "2") or "2").strip() != "2":
            raise ValueError("Only 2-player KIFU is supported")
        if not position:
            raise ValueError("Missing position")
        match = re.fullmatch(r"startpos(?:\s+(\d+))?", position.lower())
        if not match:
            raise ValueError(
                "Only 'Position: startpos 2' KIFU is currently supported "
                "for replay/branch"
            )
        if int(match.group(1) or 2) != 2:
            raise ValueError("Only 2-player position (startpos 2) is supported")

        seed = int(headers.get("Seed", "0") or 0)
        simple_payment_mode = (
            str(headers.get("SimplePaymentMode", "1")).strip() != "0"
        )
        game = Game(seed=seed)
        game.simple_payment_mode = simple_payment_mode
        states = [self._state_serializer(game)]
        normalized_moves: List[Dict[str, Any]] = []
        for move in moves:
            usi_move = str(move.get("usi", "pass"))
            action_index = find_legal_action_index_by_usi(game, usi_move)
            if action_index < 0:
                raise ValueError("pass move is not supported in replay")
            action = game.legal_actions[action_index]
            normalized_moves.append(
                {
                    "player": int(
                        move.get("player", game.board.current_player)
                    ),
                    "usi": action_to_usi(action, game=game),
                    "time_ms": move.get("time_ms"),
                    "comment": move.get("comment"),
                    "action_idx": action_index,
                }
            )
            game.apply(action)
            states.append(self._state_serializer(game))
        return {
            "parsed": parsed,
            "seed": seed,
            "simple_payment_mode": simple_payment_mode,
            "states": states,
            "moves": normalized_moves,
        }

    def load(self, path: str) -> Dict[str, Any]:
        if not os.path.isfile(path):
            raise ResourceNotFound(f"File not found: {path}")
        if not path.endswith(".kifu"):
            raise InvalidRequest("Only .kifu files are supported")
        try:
            with open(path, "r", encoding="utf-8") as stream:
                replay = self.build_replay(stream.read())
        except (ResourceNotFound, InvalidRequest):
            raise
        except Exception as error:
            raise InvalidRequest(f"Failed to load kifu: {error}") from error
        session_id = self._id_factory()
        self.replay_sessions[session_id] = {
            "id": session_id,
            "path": path,
            "filename": os.path.basename(path),
            **replay,
        }
        return {
            "session_id": session_id,
            "filename": os.path.basename(path),
            "total_steps": len(replay["states"]),
            "num_moves": len(replay["moves"]),
        }

    def get_step(self, session_id: str, step: int) -> Dict[str, Any]:
        session = self.replay_sessions.get(session_id)
        if session is None:
            raise ResourceNotFound("KIFU session not found")
        states = session["states"]
        if step < 0 or step >= len(states):
            raise InvalidRequest("Invalid step")
        move_info = None
        if step > 0 and step - 1 < len(session["moves"]):
            move_info = session["moves"][step - 1]
        return {
            "state": states[step],
            "step": step,
            "total_steps": len(states),
            "last_move": move_info,
            "headers": session["parsed"].get("headers", {}),
            "result": session["parsed"].get("result", ""),
        }

    def branch(self, session_id: str, step: int) -> Dict[str, Any]:
        session = self.replay_sessions.get(session_id)
        if session is None:
            raise ResourceNotFound("KIFU session not found")
        if step < 0 or step > len(session["moves"]):
            raise InvalidRequest("Invalid step")
        game = Game(seed=int(session["seed"]))
        game.simple_payment_mode = bool(session["simple_payment_mode"])
        stored_moves: List[Dict[str, Any]] = []
        for move in session["moves"][:step]:
            action_index = find_legal_action_index_by_usi(
                game, str(move["usi"])
            )
            if action_index < 0:
                raise InvalidRequest(
                    "Replay contains unsupported pass move"
                )
            action = game.legal_actions[action_index]
            stored_moves.append(
                {
                    "turn": int(game.board.turn),
                    "player": int(game.board.current_player),
                    "usi": action_to_usi(action, game=game),
                    "time_ms": move.get("time_ms"),
                    "comment": move.get("comment"),
                }
            )
            game.apply(action)

        new_session_id = self._id_factory()
        self.games.sessions[new_session_id] = game
        headers = dict(session["parsed"].get("headers", {}))
        seed = int(session["seed"])
        self.games.set_session_record(
            new_session_id,
            seed=seed,
            simple_payment_mode=bool(session["simple_payment_mode"]),
            initial_spn=game_to_spn(Game(seed=seed)),
            player0=headers.get("Player0", "Player0"),
            player1=headers.get("Player1", "Player1"),
        )
        self.games.records[new_session_id]["moves"] = stored_moves
        return {"session_id": new_session_id, "replayed_moves": step}
