from fastapi import FastAPI, HTTPException
from typing import Any, Dict, List, Optional
from pydantic import BaseModel
import uuid
import os
import glob
import time
import re
from .schemas import (
    GameStateSchema, BoardSchema, PlayerSchema, ActionSchema,
    ActionType
)
from .. import Game, Action, ActionType as CoreActionType
from .replay import replay_router
from .usi_kifu import (
    action_to_usi,
    find_legal_action_index_by_usi,
    build_kifu_text,
    parse_kifu_text,
    game_to_spn,
    now_iso,
)

app = FastAPI(title="Splendor Engine API")
app.include_router(replay_router)

# In-memory game sessions
sessions: Dict[str, Game] = {}
# KIFU tracking per session
session_records: Dict[str, Dict[str, Any]] = {}
# KIFU replay sessions
kifu_sessions: Dict[str, Dict[str, Any]] = {}


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
    n = re.sub(r"[^0-9A-Za-z._-]+", "_", name.strip())
    if not n:
        n = "game"
    if not n.endswith(".kifu"):
        n += ".kifu"
    return n


def _session_meta_defaults(session_id: str) -> Dict[str, Any]:
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


def _set_session_record(
    session_id: str,
    *,
    seed: int,
    simple_payment_mode: bool,
    initial_spn: str,
    player0: str = "Player0",
    player1: str = "Player1",
) -> None:
    rec = _session_meta_defaults(session_id)
    rec["seed"] = int(seed)
    rec["simple_payment_mode"] = bool(simple_payment_mode)
    rec["initial_spn"] = initial_spn
    rec["meta"]["Player0"] = player0
    rec["meta"]["Player1"] = player1
    rec["meta"]["Seed"] = str(int(seed))
    rec["meta"]["SimplePaymentMode"] = "1" if simple_payment_mode else "0"
    session_records[session_id] = rec


def _append_kifu_move(session_id: str, game: Game, action: Action,
                      time_ms: Optional[int] = None,
                      comment: Optional[str] = None) -> None:
    rec = session_records.get(session_id)
    if not rec:
        return
    rec["moves"].append({
        "turn": int(game.board.turn),
        "player": int(game.board.current_player),
        "usi": action_to_usi(action, game=game),
        "time_ms": int(time_ms) if time_ms is not None else None,
        "comment": comment if comment else None,
    })


def _result_from_game(game: Game) -> str:
    winner = int(game.board.winner)
    if winner == -2:
        return "DRAW"
    if winner >= 0:
        return f"P{winner}_WIN"
    return "ONGOING"


def _scores_from_game(game: Game) -> Optional[List[int]]:
    try:
        return [int(v) for v in game.scores()]
    except Exception:
        return None


def _build_kifu_text_for_session(session_id: str, override: Optional[KifuMetaUpdate] = None) -> str:
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")
    game = sessions[session_id]
    rec = session_records.get(session_id)
    if not rec:
        raise HTTPException(status_code=404, detail="KIFU record not found for session")

    headers = dict(rec.get("meta", {}))
    if override:
        if override.date is not None:
            headers["Date"] = override.date
        if override.event is not None:
            headers["Event"] = override.event
        if override.round is not None:
            headers["Round"] = override.round
        if override.player0 is not None:
            headers["Player0"] = override.player0
        if override.player1 is not None:
            headers["Player1"] = override.player1
        if override.player0_type is not None:
            headers["Player0Type"] = override.player0_type
        if override.player1_type is not None:
            headers["Player1Type"] = override.player1_type
        if override.comment is not None:
            headers["Comment"] = override.comment
        if override.tags is not None:
            headers["Tags"] = override.tags

    if "Date" not in headers:
        headers["Date"] = now_iso()

    # Use startpos+Seed for replay/branch compatibility.
    position = "startpos 2"
    if rec.get("initial_spn"):
        headers["InitialSPN"] = str(rec.get("initial_spn"))
    result = _result_from_game(game)
    final_scores = _scores_from_game(game)
    total_turns = int(game.board.turn)

    return build_kifu_text(
        headers=headers,
        position=position,
        moves=rec.get("moves", []),
        result=result,
        final_scores=final_scores,
        total_turns=total_turns,
    )


def core_to_schema_action(game: Game, a: Action) -> ActionSchema:
    return ActionSchema(
        type=ActionType(int(a.type)),
        take=list(a.take) if a.type in [CoreActionType.TAKE_DIFFERENT, CoreActionType.TAKE_SAME] else None,
        card_id=a.card_id if a.card_id != -1 else None,
        deck_level=a.deck_level if a.deck_level != -1 else None,
        from_reserved=a.from_reserved if a.type == CoreActionType.PURCHASE else None,
        gold_as=list(a.gold_as) if a.type == CoreActionType.PURCHASE else None,
        return_gems=list(a.return_gems) if any(a.return_gems) else None,
        noble_choice=a.noble_choice if a.noble_choice != -1 else None,
        usi=action_to_usi(a, game=game),
    )


def get_game_state(game: Game) -> GameStateSchema:
    board = game.board

    board_schema = BoardSchema(
        bank=list(board.bank),
        visible_cards=[list(row) for row in board.visible],
        deck_counts=[len(d) for d in board.decks],
        nobles=list(board.nobles),
        current_player=board.current_player,
        turn=board.turn,
        waiting_noble=board.waiting_noble,
        game_over=game.is_game_over(),
        winner=board.winner,
    )

    players_schema = []
    for i in range(2):
        p = board.players[i]
        players_schema.append(PlayerSchema(
            index=i,
            gems=list(p.gems),
            bonuses=list(p.bonuses),
            points=p.points,
            reserved_cards=[cid for cid in p.reserved if cid != -1],
            purchased_cards=list(p.purchased_cards),
            acquired_nobles=list(p.acquired_nobles),
        ))

    legal_actions = [core_to_schema_action(game, a) for a in game.legal_actions]

    return GameStateSchema(
        board=board_schema,
        players=players_schema,
        legal_actions=legal_actions,
    )


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
    session_id = str(uuid.uuid4())
    game = Game(seed=seed)
    game.simple_payment_mode = simple_payment_mode
    sessions[session_id] = game
    _set_session_record(
        session_id,
        seed=seed,
        simple_payment_mode=simple_payment_mode,
        initial_spn=game_to_spn(game),
        player0=player0_name,
        player1=player1_name,
    )
    return {"session_id": session_id}


@app.post("/game/{session_id}/kifu_meta")
async def update_kifu_meta(session_id: str, req: KifuMetaUpdate):
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")
    rec = session_records.get(session_id)
    if not rec:
        raise HTTPException(status_code=404, detail="KIFU record not found for session")

    meta = rec["meta"]
    if req.date is not None:
        meta["Date"] = req.date
    if req.event is not None:
        meta["Event"] = req.event
    if req.round is not None:
        meta["Round"] = req.round
    if req.player0 is not None:
        meta["Player0"] = req.player0
    if req.player1 is not None:
        meta["Player1"] = req.player1
    if req.player0_type is not None:
        meta["Player0Type"] = req.player0_type
    if req.player1_type is not None:
        meta["Player1Type"] = req.player1_type
    if req.comment is not None:
        meta["Comment"] = req.comment
    if req.tags is not None:
        meta["Tags"] = req.tags

    return {"ok": True}


@app.get("/game/{session_id}", response_model=GameStateSchema)
async def get_state(session_id: str):
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")
    return get_game_state(sessions[session_id])


@app.post("/game/{session_id}/action", response_model=GameStateSchema)
async def apply_action(
    session_id: str,
    action_idx: int,
    time_ms: int = None,
    comment: str = None,
):
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")

    game = sessions[session_id]
    legals = game.legal_actions

    if action_idx < 0 or action_idx >= len(legals):
        raise HTTPException(status_code=400, detail="Invalid action index")

    chosen = legals[action_idx]
    _append_kifu_move(session_id, game, chosen, time_ms=time_ms, comment=comment)
    game.apply(chosen)
    return get_game_state(game)


@app.post("/game/{session_id}/action_usi", response_model=Dict[str, Any])
async def apply_action_usi(session_id: str, req: ActionUsiRequest):
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")

    game = sessions[session_id]
    try:
        idx = find_legal_action_index_by_usi(game, req.usi_move)
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))

    if idx < 0:
        raise HTTPException(status_code=400, detail="pass is not legal in current state")

    action = game.legal_actions[idx]
    canonical_usi = action_to_usi(action, game=game)
    _append_kifu_move(session_id, game, action, time_ms=req.time_ms, comment=req.comment)
    game.apply(action)
    return {
        "action_idx": idx,
        "action_usi": canonical_usi,
        "state": get_game_state(game),
    }


@app.post("/game/{session_id}/undo", response_model=GameStateSchema)
async def undo_action(session_id: str):
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")

    game = sessions[session_id]
    game.undo()
    rec = session_records.get(session_id)
    if rec and rec.get("moves"):
        rec["moves"] = rec["moves"][:-1]
    return get_game_state(game)


@app.post("/game/{session_id}/ai_move", response_model=Dict[str, Any])
async def get_ai_move(
    session_id: str,
    ai_type: str = "greedy",
    time_limit: float = 2.0,
    use_determinization: bool = False,
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
        use_determinization: Whether to use determinization for MCTS (default: False)
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
    if session_id not in sessions:
        raise HTTPException(status_code=404, detail="Session not found")

    from .ai_manager import AIManager
    try:
        game = sessions[session_id]
        ai_manager = AIManager.get_instance()

        # Pack AlphaZero options
        az_options = {
            'fpu': fpu,
            'forced_playouts': forced_playouts,
            'ratio_fullMCTS': ratio_full_mcts,
            'prob_fullMCTS': prob_full_mcts,
            'temperature': [temperature_early, temperature_late],
            'cpuct': cpuct,
            'dirichletAlpha': dirichlet_alpha,
            'model_path': model_path,
        }

        # Distilled/search AIs must explicitly run with fixed-count search budget from UI selection.
        # Prevent silent fallback to raw NN inference when simulations are missing.
        if ai_type in ("deepsets", "set_transformer", "nnue") and (num_simulations is None or num_simulations <= 0):
            if ai_type == "set_transformer":
                ai_name = "SetTransformer"
            elif ai_type == "nnue":
                ai_name = "NNUE"
            else:
                ai_name = "DeepSets"
            raise HTTPException(
                status_code=400,
                detail=f"{ai_name} requires a positive num_simulations (search budget)."
            )

        action_start = time.perf_counter()
        action_idx = ai_manager.get_best_action(
            game,
            ai_type=ai_type,
            time_limit=time_limit,
            use_determinization=use_determinization,
            num_simulations=num_simulations,
            az_options=az_options,
        )
        action_elapsed_ms = (time.perf_counter() - action_start) * 1000.0
        debug = getattr(ai_manager, "_last_action_debug", {}) or {}
        used_mode = ai_type
        if ai_type == "deepsets":
            used_mode = "deepsets_mcts"
        elif ai_type == "set_transformer":
            used_mode = "set_transformer_mcts"
        elif ai_type == "nnue":
            used_mode = "nnue_ab"
        elif ai_type == "alphazero":
            used_mode = "alphazero_mcts" if num_simulations else "alphazero_time"
        elif ai_type == "genbu":
            used_mode = "genbu_mcts" if num_simulations else "genbu_time"
        if isinstance(debug.get("used_mode"), str):
            used_mode = debug["used_mode"]

        used_simulations = num_simulations
        if debug.get("actual_simulations") is not None:
            used_simulations = int(debug["actual_simulations"])

        elapsed_ms = action_elapsed_ms
        if debug.get("elapsed_ms") is not None:
            elapsed_ms = float(debug["elapsed_ms"])

        action_usi = None
        if 0 <= action_idx < len(game.legal_actions):
            action_usi = action_to_usi(game.legal_actions[action_idx], game=game)

        return {
            "action_idx": action_idx,
            "action_usi": action_usi,
            "used_mode": used_mode,
            "used_simulations": used_simulations,
            "elapsed_ms": int(elapsed_ms),
        }
    except HTTPException:
        raise
    except Exception as e:
        import traceback
        traceback.print_exc()
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/game/{session_id}/export_kifu", response_model=Dict[str, Any])
async def export_game_kifu(session_id: str, req: Optional[KifuMetaUpdate] = None):
    kifu_text = _build_kifu_text_for_session(session_id, override=req)
    ts = time.strftime("%Y%m%d_%H%M%S")
    filename = _safe_filename(f"game_{session_id[:8]}_{ts}")
    out_path = os.path.join(_kifu_dir(), filename)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(kifu_text)
    return {"path": out_path, "filename": filename, "saved": True}


@app.post("/kifu/save_text", response_model=Dict[str, Any])
async def save_kifu_text(req: SaveKifuTextRequest):
    filename = _safe_filename(req.filename)
    out_path = os.path.join(_kifu_dir(), filename)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(req.text)
    return {"path": out_path, "filename": filename, "saved": True}


@app.get("/kifu/files", response_model=Dict[str, List[Dict[str, Any]]])
async def list_kifu_files():
    files: List[Dict[str, Any]] = []
    for path in sorted(glob.glob(os.path.join(_kifu_dir(), "*.kifu"))):
        st = os.stat(path)
        files.append({
            "filename": os.path.basename(path),
            "path": path,
            "size": int(st.st_size),
            "mtime": int(st.st_mtime),
        })
    return {"files": files}


def _build_replay_from_kifu_text(text: str) -> Dict[str, Any]:
    parsed = parse_kifu_text(text)
    headers = parsed.get("headers", {})
    position = str(parsed.get("position", "")).strip()
    moves = parsed.get("moves", [])

    players_raw = str(headers.get("Players", "2") or "2").strip()
    if players_raw != "2":
        raise ValueError("Only 2-player KIFU is supported")

    if not position:
        raise ValueError("Missing position")

    mpos = re.fullmatch(r"startpos(?:\s+(\d+))?", position.lower())
    if not mpos:
        raise ValueError("Only 'Position: startpos 2' KIFU is currently supported for replay/branch")
    pos_players = int(mpos.group(1) or 2)
    if pos_players != 2:
        raise ValueError("Only 2-player position (startpos 2) is supported")

    seed = int(headers.get("Seed", "0") or 0)
    simple_payment_mode = str(headers.get("SimplePaymentMode", "1")).strip() != "0"

    game = Game(seed=seed)
    game.simple_payment_mode = simple_payment_mode

    states: List[GameStateSchema] = [get_game_state(game)]
    normalized_moves: List[Dict[str, Any]] = []

    for mv in moves:
        usi = str(mv.get("usi", "pass"))
        idx = find_legal_action_index_by_usi(game, usi)
        if idx < 0:
            raise ValueError("pass move is not supported in replay")
        action = game.legal_actions[idx]
        normalized = {
            "player": int(mv.get("player", game.board.current_player)),
            "usi": action_to_usi(action, game=game),
            "time_ms": mv.get("time_ms"),
            "comment": mv.get("comment"),
            "action_idx": idx,
        }
        normalized_moves.append(normalized)
        game.apply(action)
        states.append(get_game_state(game))

    return {
        "parsed": parsed,
        "seed": seed,
        "simple_payment_mode": simple_payment_mode,
        "states": states,
        "moves": normalized_moves,
    }


@app.post("/kifu/load", response_model=Dict[str, Any])
async def load_kifu(req: LoadKifuRequest):
    if not os.path.isfile(req.path):
        raise HTTPException(status_code=404, detail=f"File not found: {req.path}")
    if not req.path.endswith(".kifu"):
        raise HTTPException(status_code=400, detail="Only .kifu files are supported")

    try:
        with open(req.path, "r", encoding="utf-8") as f:
            text = f.read()
        replay = _build_replay_from_kifu_text(text)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Failed to load kifu: {e}")

    sid = str(uuid.uuid4())
    kifu_sessions[sid] = {
        "id": sid,
        "path": req.path,
        "filename": os.path.basename(req.path),
        **replay,
    }

    return {
        "session_id": sid,
        "filename": os.path.basename(req.path),
        "total_steps": len(replay["states"]),
        "num_moves": len(replay["moves"]),
    }


@app.get("/kifu/{session_id}/step/{step}", response_model=Dict[str, Any])
async def get_kifu_step(session_id: str, step: int):
    sess = kifu_sessions.get(session_id)
    if not sess:
        raise HTTPException(status_code=404, detail="KIFU session not found")

    states = sess["states"]
    if step < 0 or step >= len(states):
        raise HTTPException(status_code=400, detail="Invalid step")

    move_info = None
    if step > 0 and step - 1 < len(sess["moves"]):
        move_info = sess["moves"][step - 1]

    return {
        "state": states[step],
        "step": step,
        "total_steps": len(states),
        "last_move": move_info,
        "headers": sess["parsed"].get("headers", {}),
        "result": sess["parsed"].get("result", ""),
    }


@app.post("/kifu/{session_id}/branch", response_model=Dict[str, Any])
async def branch_from_kifu(session_id: str, req: BranchRequest):
    sess = kifu_sessions.get(session_id)
    if not sess:
        raise HTTPException(status_code=404, detail="KIFU session not found")

    step = int(req.step)
    if step < 0 or step > len(sess["moves"]):
        raise HTTPException(status_code=400, detail="Invalid step")

    # Rebuild live game by replaying moves up to step.
    game = Game(seed=int(sess["seed"]))
    game.simple_payment_mode = bool(sess["simple_payment_mode"])

    replayed_moves = sess["moves"][:step]
    stored_moves: List[Dict[str, Any]] = []
    for mv in replayed_moves:
        idx = find_legal_action_index_by_usi(game, str(mv["usi"]))
        if idx < 0:
            raise HTTPException(status_code=400, detail="Replay contains unsupported pass move")
        action = game.legal_actions[idx]
        stored_moves.append({
            "turn": int(game.board.turn),
            "player": int(game.board.current_player),
            "usi": action_to_usi(action, game=game),
            "time_ms": mv.get("time_ms"),
            "comment": mv.get("comment"),
        })
        game.apply(action)

    new_session_id = str(uuid.uuid4())
    sessions[new_session_id] = game

    headers = dict(sess["parsed"].get("headers", {}))
    _set_session_record(
        new_session_id,
        seed=int(sess["seed"]),
        simple_payment_mode=bool(sess["simple_payment_mode"]),
        initial_spn=game_to_spn(Game(seed=int(sess["seed"]))),
        player0=headers.get("Player0", "Player0"),
        player1=headers.get("Player1", "Player1"),
    )
    session_records[new_session_id]["moves"] = stored_moves

    return {"session_id": new_session_id, "replayed_moves": step}


@app.get("/models", response_model=Dict[str, List[Dict[str, str]]])
async def list_models():
    """
    List available .pt model files for AlphaZero.

    Returns a list of model files found in common directories.
    """
    # Base directory (splendorgui root)
    base_dir = _project_root()

    # Directories to search for models
    search_dirs = [
        "alphazero-general/models3",
        "alphazero-general/temp",
        "alphazero-general/models",
        "models",
    ]

    models = []
    seen_paths = set()

    for search_dir in search_dirs:
        full_dir = os.path.join(base_dir, search_dir)
        if not os.path.exists(full_dir):
            continue

        # Find .pt and .pth.tar files
        for pattern in ["*.pt", "*.pth.tar"]:
            for filepath in glob.glob(os.path.join(full_dir, pattern)):
                if filepath not in seen_paths:
                    seen_paths.add(filepath)
                    # Get relative path from base_dir for display
                    rel_path = os.path.relpath(filepath, base_dir)
                    filename = os.path.basename(filepath)
                    models.append({
                        "path": filepath,
                        "name": filename,
                        "display": rel_path,
                        "dir": search_dir,
                    })

    # Sort by directory priority, then filename
    models.sort(key=lambda m: (search_dirs.index(m["dir"]) if m["dir"] in search_dirs else 999, m["name"]))

    return {"models": models}
