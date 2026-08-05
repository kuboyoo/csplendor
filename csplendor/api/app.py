import logging
import os
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

from .. import Action, Game
from .ai_provider import AIRequest, get_ai_provider
from .application_errors import ApplicationError
from .game_presenter import (
    core_to_schema_action as _present_action,
)
from .game_presenter import (
    get_game_state as _present_game,
)
from .game_service import GameSessionService
from .kifu_service import KifuApplicationService
from .replay import replay_router
from .schemas import ActionSchema, GameStateSchema
from .stores import InMemoryStore
from .usi_serializer import action_to_usi

app = FastAPI(title="Splendor Engine API")
app.include_router(replay_router)
logger = logging.getLogger(__name__)

# Dictionary-compatible aliases are retained for existing embedders and tests.
# Application code accesses them through the injected store interfaces.
sessions: InMemoryStore[Game] = InMemoryStore()
session_records: InMemoryStore[Dict[str, Any]] = InMemoryStore()
kifu_sessions: InMemoryStore[Dict[str, Any]] = InMemoryStore()
_game_service = GameSessionService(sessions, session_records)


class KifuMetaUpdate(BaseModel):
    date: Optional[str] = None
    event: Optional[str] = None
    round: Optional[str] = None
    player0: Optional[str] = None
    player1: Optional[str] = None
    player0_type: Optional[str] = None
    player1_type: Optional[str] = None
    comment: Optional[str] = None
    tags: Optional[str] = None


class SaveKifuTextRequest(BaseModel):
    filename: str
    text: str


class LoadKifuRequest(BaseModel):
    path: str


class BranchRequest(BaseModel):
    step: int


class ActionUsiRequest(BaseModel):
    usi_move: str
    time_ms: Optional[int] = None
    comment: Optional[str] = None


def _project_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))


def _kifu_dir() -> str:
    d = os.path.join(_project_root(), "data", "kifu")
    os.makedirs(d, exist_ok=True)
    return d


def _safe_filename(name: str) -> str:
    return KifuApplicationService.safe_filename(name)


def _session_meta_defaults(session_id: str) -> Dict[str, Any]:
    return _game_service.session_record_defaults(session_id)


def _set_session_record(
    session_id: str,
    *,
    seed: int,
    simple_payment_mode: bool,
    initial_spn: str,
    player0: str = "Player0",
    player1: str = "Player1",
) -> None:
    _game_service.set_session_record(
        session_id,
        seed=seed,
        simple_payment_mode=simple_payment_mode,
        initial_spn=initial_spn,
        player0=player0,
        player1=player1,
    )


def _append_kifu_move(session_id: str, game: Game, action: Action,
                      time_ms: Optional[int] = None,
                      comment: Optional[str] = None) -> None:
    _game_service.append_move(
        session_id, game, action, time_ms=time_ms, comment=comment
    )


def _result_from_game(game: Game) -> str:
    return _game_service.result_from_game(game)


def _scores_from_game(game: Game) -> Optional[List[int]]:
    return _game_service.scores_from_game(game)


def _build_kifu_text_for_session(
    session_id: str, override: Optional[KifuMetaUpdate] = None
) -> str:
    try:
        values = _meta_values(override) if override is not None else None
        return _game_service.build_kifu_text(session_id, values)
    except ApplicationError as error:
        raise _http_error(error) from error


def core_to_schema_action(game: Game, a: Action) -> ActionSchema:
    return _present_action(game, a)


def get_game_state(game: Game) -> GameStateSchema:
    return _present_game(game)


_kifu_service = KifuApplicationService(
    _game_service,
    kifu_sessions,
    kifu_dir=_kifu_dir,
    state_serializer=get_game_state,
)


def _http_error(error: ApplicationError) -> HTTPException:
    return HTTPException(status_code=error.status_code, detail=error.detail)


def _meta_values(update: KifuMetaUpdate) -> Dict[str, Optional[str]]:
    return {
        "Date": update.date,
        "Event": update.event,
        "Round": update.round,
        "Player0": update.player0,
        "Player1": update.player1,
        "Player0Type": update.player0_type,
        "Player1Type": update.player1_type,
        "Comment": update.comment,
        "Tags": update.tags,
    }


@app.post("/game", response_model=Dict[str, str])
async def create_game(
    seed: int = 0,
    simple_payment_mode: bool = True,
    player0_name: str = "Player0",
    player1_name: str = "Player1",
):
    """
    Create a new game session.

    Args:
        seed: Random seed for game initialization (0 = random)
        simple_payment_mode: If True (Casual mode), use simplified payment patterns
                            (minimize gold usage). If False (Advanced mode),
                            allow all payment combinations.
    """
    session_id = _game_service.create_game(
        seed=seed,
        simple_payment_mode=simple_payment_mode,
        player0_name=player0_name,
        player1_name=player1_name,
    )
    return {"session_id": session_id}


@app.post("/game/{session_id}/kifu_meta")
async def update_kifu_meta(session_id: str, req: KifuMetaUpdate):
    try:
        _game_service.update_meta(session_id, _meta_values(req))
    except ApplicationError as error:
        raise _http_error(error) from error
    return {"ok": True}


@app.get("/game/{session_id}", response_model=GameStateSchema)
async def get_state(session_id: str):
    try:
        return get_game_state(_game_service.require_game(session_id))
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/game/{session_id}/action", response_model=GameStateSchema)
async def apply_action(
    session_id: str,
    action_idx: int,
    time_ms: int = None,
    comment: str = None,
):
    try:
        game = _game_service.apply_action(
            session_id,
            action_idx,
            time_ms=time_ms,
            comment=comment,
        )
        return get_game_state(game)
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/game/{session_id}/action_usi", response_model=Dict[str, Any])
async def apply_action_usi(session_id: str, req: ActionUsiRequest):
    try:
        action_index, canonical_usi, game = _game_service.apply_usi(
            session_id,
            req.usi_move,
            time_ms=req.time_ms,
            comment=req.comment,
        )
        return {
            "action_idx": action_index,
            "action_usi": canonical_usi,
            "state": get_game_state(game),
        }
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/game/{session_id}/undo", response_model=GameStateSchema)
async def undo_action(session_id: str):
    try:
        return get_game_state(_game_service.undo(session_id))
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/game/{session_id}/ai_move", response_model=Dict[str, Any])
async def get_ai_move(
    session_id: str,
    ai_type: str = "greedy",
    time_limit: float = 2.0,
    use_determinization: bool = True,
    num_simulations: int = None,
    # AlphaZero advanced options - inference defaults (optimized for strongest play)
    fpu: float = 0.0,              # Keep same as training
    forced_playouts: bool = False, # OFF for inference
    ratio_full_mcts: int = 5,      # Keep same as training
    prob_full_mcts: float = 0.25,  # Keep same as training
    temperature_early: float = 0.1,  # Low for deterministic play
    temperature_late: float = 0.1,   # Low for deterministic play
    cpuct: float = 1.5,            # Keep same as training
    dirichlet_alpha: float = 0.03, # Effectively disabled for inference
    model_path: str = None,        # Path to .pt model file (optional)
):
    """
    Get AI move for the current game state.

    Args:
        session_id: Game session ID
        ai_type: AI type - "mcts", "greedy", "genbu", "alphazero", "deepsets", "set_transformer", or "nnue"
        time_limit: Max thinking time in seconds (default: 2.0)
        use_determinization: Whether to use determinization for MCTS (default: True)
        num_simulations: Fixed number of MCTS simulations (optional, overrides time_limit for alphazero)
        model_path: Path to .pt model file (optional, uses default if not specified)

        AlphaZero advanced options:
        fpu: First Play Urgency value (negative=absolute, positive=parent reduction)
        forced_playouts: Enable forced playouts for high-policy moves
        ratio_full_mcts: Ratio between full and fast MCTS simulations
        prob_full_mcts: Probability of choosing full MCTS exploration
        temperature_early: Softmax temperature for early game moves
        temperature_late: Softmax temperature for late game moves
        cpuct: PUCT exploration constant
        dirichlet_alpha: Dirichlet noise alpha for root exploration
    """
    try:
        game = _game_service.require_game(session_id)
    except ApplicationError as error:
        raise _http_error(error) from error

    allowed_ai_types = {
        "mcts",
        "greedy",
        "genbu",
        "alphazero",
        "deepsets",
        "set_transformer",
        "nnue",
    }
    if ai_type not in allowed_ai_types:
        raise HTTPException(status_code=400, detail=f"Unknown AI type: {ai_type}")

    # Distilled/search AIs must explicitly run with a fixed-count search
    # budget. Validate this before loading any optional model dependency.
    if ai_type in ("deepsets", "set_transformer", "nnue") and (
        num_simulations is None or num_simulations <= 0
    ):
        ai_name = {
            "deepsets": "DeepSets",
            "set_transformer": "SetTransformer",
            "nnue": "NNUE",
        }[ai_type]
        raise HTTPException(
            status_code=400,
            detail=f"{ai_name} requires a positive num_simulations "
            "(search budget).",
        )

    try:
        decision = get_ai_provider().choose_action(
            game,
            AIRequest(
                ai_type=ai_type,
                time_limit=time_limit,
                use_determinization=use_determinization,
                num_simulations=num_simulations,
                options={
                    "fpu": fpu,
                    "forced_playouts": forced_playouts,
                    "ratio_fullMCTS": ratio_full_mcts,
                    "prob_fullMCTS": prob_full_mcts,
                    "temperature": [temperature_early, temperature_late],
                    "cpuct": cpuct,
                    "dirichletAlpha": dirichlet_alpha,
                    "model_path": model_path,
                },
            ),
        )
        action_usi = None
        if 0 <= decision.action_index < len(game.legal_actions):
            action_usi = action_to_usi(
                game.legal_actions[decision.action_index], game=game
            )
        return {
            "action_idx": decision.action_index,
            "action_usi": action_usi,
            "used_mode": decision.used_mode,
            "used_simulations": decision.used_simulations,
            "elapsed_ms": decision.elapsed_ms,
        }
    except ApplicationError as error:
        raise _http_error(error) from error
    except Exception as error:
        logger.exception(
            "AI provider failed",
            extra={"event": "ai_provider_failure", "ai_type": ai_type},
        )
        raise HTTPException(status_code=500, detail=str(error)) from error


@app.post("/game/{session_id}/export_kifu", response_model=Dict[str, Any])
async def export_game_kifu(session_id: str, req: Optional[KifuMetaUpdate] = None):
    try:
        override = _meta_values(req) if req is not None else None
        return _kifu_service.export_session(session_id, override)
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/kifu/save_text", response_model=Dict[str, Any])
async def save_kifu_text(req: SaveKifuTextRequest):
    return _kifu_service.save_text(req.filename, req.text)


@app.get("/kifu/files", response_model=Dict[str, List[Dict[str, Any]]])
async def list_kifu_files():
    return {"files": _kifu_service.list_files()}


def _build_replay_from_kifu_text(text: str) -> Dict[str, Any]:
    return _kifu_service.build_replay(text)


@app.post("/kifu/load", response_model=Dict[str, Any])
async def load_kifu(req: LoadKifuRequest):
    try:
        return _kifu_service.load(req.path)
    except ApplicationError as error:
        raise _http_error(error) from error


@app.get("/kifu/{session_id}/step/{step}", response_model=Dict[str, Any])
async def get_kifu_step(session_id: str, step: int):
    try:
        return _kifu_service.get_step(session_id, step)
    except ApplicationError as error:
        raise _http_error(error) from error


@app.post("/kifu/{session_id}/branch", response_model=Dict[str, Any])
async def branch_from_kifu(session_id: str, req: BranchRequest):
    try:
        return _kifu_service.branch(session_id, int(req.step))
    except ApplicationError as error:
        raise _http_error(error) from error


@app.get("/models", response_model=Dict[str, List[Dict[str, str]]])
async def list_models():
    """
    List available .pt model files for AlphaZero.

    Returns a list of model files found in common directories.
    """
    return {"models": get_ai_provider().list_models()}
