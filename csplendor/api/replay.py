"""
Replay API for viewing recorded games from .pkl files.

The pkl files contain per-turn examples from alphazero-general-ori's data
generation. Each example has a ``board_ori`` (56×7 int8 matrix in canonical
form) plus MCTS policy and metadata.

This module decodes the ori board matrix back into a ``GameStateSchema``
that the frontend ``GameBoard`` component can render.
"""

from fastapi import APIRouter, HTTPException
from typing import Dict, List, Optional
from pydantic import BaseModel
import os
import glob
import pickle
import uuid

import numpy as np

from .schemas import (
    GameStateSchema, BoardSchema, PlayerSchema, ActionSchema,
)

replay_router = APIRouter(prefix="/replay", tags=["replay"])

# ── Card / Noble lookup tables ──────────────────────────────────────────
# These replicate the card data from the frontend's gameData.ts so that we
# can map ori board vectors back to card IDs.

# fmt: off
_CARDS = [
    # Level 1 – Sapphire (bonus=1)
    {"id": 0, "level": 1, "points": 0, "bonus": 1, "cost": [0,0,0,0,3]},
    {"id": 1, "level": 1, "points": 0, "bonus": 1, "cost": [1,0,0,0,2]},
    {"id": 2, "level": 1, "points": 0, "bonus": 1, "cost": [0,0,2,0,2]},
    {"id": 3, "level": 1, "points": 0, "bonus": 1, "cost": [1,0,2,2,0]},
    {"id": 4, "level": 1, "points": 0, "bonus": 1, "cost": [0,1,3,1,0]},
    {"id": 5, "level": 1, "points": 0, "bonus": 1, "cost": [1,0,1,1,1]},
    {"id": 6, "level": 1, "points": 0, "bonus": 1, "cost": [1,0,1,2,1]},
    {"id": 7, "level": 1, "points": 1, "bonus": 1, "cost": [0,0,0,4,0]},
    # Level 1 – Ruby (bonus=3)
    {"id": 8, "level": 1, "points": 0, "bonus": 3, "cost": [3,0,0,0,0]},
    {"id": 9, "level": 1, "points": 0, "bonus": 3, "cost": [0,2,1,0,0]},
    {"id":10, "level": 1, "points": 0, "bonus": 3, "cost": [2,0,0,2,0]},
    {"id":11, "level": 1, "points": 0, "bonus": 3, "cost": [2,0,1,0,2]},
    {"id":12, "level": 1, "points": 0, "bonus": 3, "cost": [1,0,0,1,3]},
    {"id":13, "level": 1, "points": 0, "bonus": 3, "cost": [1,1,1,0,1]},
    {"id":14, "level": 1, "points": 0, "bonus": 3, "cost": [2,1,1,0,1]},
    {"id":15, "level": 1, "points": 1, "bonus": 3, "cost": [4,0,0,0,0]},
    # Level 1 – Onyx (bonus=4)
    {"id":16, "level": 1, "points": 0, "bonus": 4, "cost": [0,0,3,0,0]},
    {"id":17, "level": 1, "points": 0, "bonus": 4, "cost": [0,0,2,1,0]},
    {"id":18, "level": 1, "points": 0, "bonus": 4, "cost": [2,0,2,0,0]},
    {"id":19, "level": 1, "points": 0, "bonus": 4, "cost": [2,2,0,1,0]},
    {"id":20, "level": 1, "points": 0, "bonus": 4, "cost": [0,0,1,3,1]},
    {"id":21, "level": 1, "points": 0, "bonus": 4, "cost": [1,1,1,1,0]},
    {"id":22, "level": 1, "points": 0, "bonus": 4, "cost": [1,2,1,1,0]},
    {"id":23, "level": 1, "points": 1, "bonus": 4, "cost": [0,4,0,0,0]},
    # Level 1 – Diamond (bonus=0)
    {"id":24, "level": 1, "points": 0, "bonus": 0, "cost": [0,3,0,0,0]},
    {"id":25, "level": 1, "points": 0, "bonus": 0, "cost": [0,0,0,2,1]},
    {"id":26, "level": 1, "points": 0, "bonus": 0, "cost": [0,2,0,0,2]},
    {"id":27, "level": 1, "points": 0, "bonus": 0, "cost": [0,2,2,0,1]},
    {"id":28, "level": 1, "points": 0, "bonus": 0, "cost": [3,1,0,0,1]},
    {"id":29, "level": 1, "points": 0, "bonus": 0, "cost": [0,1,1,1,1]},
    {"id":30, "level": 1, "points": 0, "bonus": 0, "cost": [0,1,2,1,1]},
    {"id":31, "level": 1, "points": 1, "bonus": 0, "cost": [0,0,4,0,0]},
    # Level 1 – Emerald (bonus=2)
    {"id":32, "level": 1, "points": 0, "bonus": 2, "cost": [0,0,0,3,0]},
    {"id":33, "level": 1, "points": 0, "bonus": 2, "cost": [2,1,0,0,0]},
    {"id":34, "level": 1, "points": 0, "bonus": 2, "cost": [0,2,0,2,0]},
    {"id":35, "level": 1, "points": 0, "bonus": 2, "cost": [0,1,0,2,2]},
    {"id":36, "level": 1, "points": 0, "bonus": 2, "cost": [1,3,1,0,0]},
    {"id":37, "level": 1, "points": 0, "bonus": 2, "cost": [1,1,0,1,1]},
    {"id":38, "level": 1, "points": 0, "bonus": 2, "cost": [1,1,0,1,2]},
    {"id":39, "level": 1, "points": 1, "bonus": 2, "cost": [0,0,0,0,4]},
    # Level 2 – Sapphire (bonus=1)
    {"id":40, "level": 2, "points": 1, "bonus": 1, "cost": [0,2,2,3,0]},
    {"id":41, "level": 2, "points": 1, "bonus": 1, "cost": [0,2,3,0,3]},
    {"id":42, "level": 2, "points": 2, "bonus": 1, "cost": [0,5,0,0,0]},
    {"id":43, "level": 2, "points": 2, "bonus": 1, "cost": [5,3,0,0,0]},
    {"id":44, "level": 2, "points": 2, "bonus": 1, "cost": [2,0,0,1,4]},
    {"id":45, "level": 2, "points": 3, "bonus": 1, "cost": [0,6,0,0,0]},
    # Level 2 – Ruby (bonus=3)
    {"id":46, "level": 2, "points": 1, "bonus": 3, "cost": [2,0,0,2,3]},
    {"id":47, "level": 2, "points": 1, "bonus": 3, "cost": [0,3,0,2,3]},
    {"id":48, "level": 2, "points": 2, "bonus": 3, "cost": [0,0,0,0,5]},
    {"id":49, "level": 2, "points": 2, "bonus": 3, "cost": [3,0,0,0,5]},
    {"id":50, "level": 2, "points": 2, "bonus": 3, "cost": [1,4,2,0,0]},
    {"id":51, "level": 2, "points": 3, "bonus": 3, "cost": [0,0,0,6,0]},
    # Level 2 – Onyx (bonus=4)
    {"id":52, "level": 2, "points": 1, "bonus": 4, "cost": [3,2,2,0,0]},
    {"id":53, "level": 2, "points": 1, "bonus": 4, "cost": [3,0,3,0,2]},
    {"id":54, "level": 2, "points": 2, "bonus": 4, "cost": [5,0,0,0,0]},
    {"id":55, "level": 2, "points": 2, "bonus": 4, "cost": [0,0,5,3,0]},
    {"id":56, "level": 2, "points": 2, "bonus": 4, "cost": [0,1,4,2,0]},
    {"id":57, "level": 2, "points": 3, "bonus": 4, "cost": [0,0,0,0,6]},
    # Level 2 – Diamond (bonus=0)
    {"id":58, "level": 2, "points": 1, "bonus": 0, "cost": [0,0,3,2,2]},
    {"id":59, "level": 2, "points": 1, "bonus": 0, "cost": [2,3,0,3,0]},
    {"id":60, "level": 2, "points": 2, "bonus": 0, "cost": [0,0,0,5,0]},
    {"id":61, "level": 2, "points": 2, "bonus": 0, "cost": [0,0,0,5,3]},
    {"id":62, "level": 2, "points": 2, "bonus": 0, "cost": [0,0,1,4,2]},
    {"id":63, "level": 2, "points": 3, "bonus": 0, "cost": [6,0,0,0,0]},
    # Level 2 – Emerald (bonus=2)
    {"id":64, "level": 2, "points": 1, "bonus": 2, "cost": [2,3,0,0,2]},
    {"id":65, "level": 2, "points": 1, "bonus": 2, "cost": [3,0,2,3,0]},
    {"id":66, "level": 2, "points": 2, "bonus": 2, "cost": [0,0,5,0,0]},
    {"id":67, "level": 2, "points": 2, "bonus": 2, "cost": [0,5,3,0,0]},
    {"id":68, "level": 2, "points": 2, "bonus": 2, "cost": [4,2,0,0,1]},
    {"id":69, "level": 2, "points": 3, "bonus": 2, "cost": [0,0,6,0,0]},
    # Level 3 – Sapphire (bonus=1)
    {"id":70, "level": 3, "points": 3, "bonus": 1, "cost": [3,0,3,3,5]},
    {"id":71, "level": 3, "points": 4, "bonus": 1, "cost": [7,0,0,0,0]},
    {"id":72, "level": 3, "points": 4, "bonus": 1, "cost": [6,3,0,0,3]},
    {"id":73, "level": 3, "points": 5, "bonus": 1, "cost": [7,3,0,0,0]},
    # Level 3 – Ruby (bonus=3)
    {"id":74, "level": 3, "points": 3, "bonus": 3, "cost": [3,5,3,0,3]},
    {"id":75, "level": 3, "points": 4, "bonus": 3, "cost": [0,0,7,0,0]},
    {"id":76, "level": 3, "points": 4, "bonus": 3, "cost": [0,3,6,3,0]},
    {"id":77, "level": 3, "points": 5, "bonus": 3, "cost": [0,0,7,3,0]},
    # Level 3 – Onyx (bonus=4)
    {"id":78, "level": 3, "points": 3, "bonus": 4, "cost": [3,3,5,3,0]},
    {"id":79, "level": 3, "points": 4, "bonus": 4, "cost": [0,0,0,7,0]},
    {"id":80, "level": 3, "points": 4, "bonus": 4, "cost": [0,0,3,6,3]},
    {"id":81, "level": 3, "points": 5, "bonus": 4, "cost": [0,0,0,7,3]},
    # Level 3 – Diamond (bonus=0)
    {"id":82, "level": 3, "points": 3, "bonus": 0, "cost": [0,3,3,5,3]},
    {"id":83, "level": 3, "points": 4, "bonus": 0, "cost": [0,0,0,0,7]},
    {"id":84, "level": 3, "points": 4, "bonus": 0, "cost": [3,0,0,3,6]},
    {"id":85, "level": 3, "points": 5, "bonus": 0, "cost": [3,0,0,0,7]},
    # Level 3 – Emerald (bonus=2)
    {"id":86, "level": 3, "points": 3, "bonus": 2, "cost": [5,3,0,3,3]},
    {"id":87, "level": 3, "points": 4, "bonus": 2, "cost": [0,7,0,0,0]},
    {"id":88, "level": 3, "points": 4, "bonus": 2, "cost": [3,6,3,0,0]},
    {"id":89, "level": 3, "points": 5, "bonus": 2, "cost": [0,7,3,0,0]},
]

_NOBLES = [
    {"id": 0, "points": 3, "requirement": [0,0,4,4,0]},
    {"id": 1, "points": 3, "requirement": [0,0,0,4,4]},
    {"id": 2, "points": 3, "requirement": [0,4,4,0,0]},
    {"id": 3, "points": 3, "requirement": [4,0,0,0,4]},
    {"id": 4, "points": 3, "requirement": [4,4,0,0,0]},
    {"id": 5, "points": 3, "requirement": [4,0,0,4,0]},  # ori noble 5 => [3,0,0,3,3]? No...
    {"id": 6, "points": 3, "requirement": [3,0,0,3,3]},
    {"id": 7, "points": 3, "requirement": [3,3,3,0,0]},
    {"id": 8, "points": 3, "requirement": [0,0,3,3,3]},
    {"id": 9, "points": 3, "requirement": [0,3,3,3,0]},
    {"id":10, "points": 3, "requirement": [3,3,0,0,3]},
    {"id":11, "points": 3, "requirement": [0,3,3,0,3]},  # ← extra noble not in ori (only 10)
]
# fmt: on

# Build lookup: (cost_tuple, bonus, points) → card_id
_card_lookup: Dict[tuple, int] = {}
for c in _CARDS:
    key = (tuple(c["cost"]), c["bonus"], c["points"])
    _card_lookup[key] = c["id"]

# Build lookup: requirement_tuple → noble_id
_noble_lookup: Dict[tuple, int] = {}
for n in _NOBLES:
    key = tuple(n["requirement"])
    _noble_lookup[key] = n["id"]


# ── In-memory replay sessions ──────────────────────────────────────────

class ReplaySession:
    """Stores a loaded replay."""

    def __init__(self, filename: str, examples: list):
        self.filename = filename
        self.examples = examples  # list of dicts from pkl
        self.total_steps = len(examples)
        # Group by game (consecutive steps with sequential turns)
        self.games: List[List[dict]] = []
        current_game: List[dict] = []
        prev_turn = -1
        for ex in examples:
            turn = ex["turn"]
            if turn <= prev_turn and current_game:
                self.games.append(current_game)
                current_game = []
            current_game.append(ex)
            prev_turn = turn
        if current_game:
            self.games.append(current_game)


_sessions: Dict[str, ReplaySession] = {}


# ── Board decoder ───────────────────────────────────────────────────────

def _card_id_from_ori(row0: np.ndarray, row1: np.ndarray) -> int:
    """
    Given two rows from the ori state (card cost row + card bonus row),
    return the matching card ID from gameData, or -1 if empty/unknown.
    """
    cost = tuple(int(x) for x in row0[:5])
    if sum(cost) == 0 and int(row0[6]) == 0:
        return -1  # empty slot
    # Determine bonus color from row1 (the column with value 1)
    bonus = -1
    for i in range(5):
        if int(row1[i]) > 0:
            bonus = i
            break
    # Map ori bonus (W=0,B=1,G=2,R=3,K=4) to gameData bonus
    # ori:      White=0  Blue=1  Green=2  Red=3  Black=4
    # gameData: Diamond=0 Sapphire=1 Emerald=2 Ruby=3 Onyx=4
    # Mapping: same indices!
    points = int(row0[6]) if int(row0[6]) > 0 else (int(row1[6]) if int(row1[6]) > 0 else 0)
    key = (cost, bonus, points)
    return _card_lookup.get(key, -1)


def _noble_id_from_ori(row: np.ndarray) -> int:
    """
    Given a noble row [req_w, req_b, req_g, req_r, req_k, 0, points],
    return the matching noble ID, or -1 if empty.
    """
    req = tuple(int(x) for x in row[:5])
    if sum(req) == 0:
        return -1
    return _noble_lookup.get(req, -1)


def _decode_board_ori(
    board_ori: np.ndarray,
    player_perspective: int,
    turn: int,
    value_target: float,
    final_turn: int,
) -> dict:
    """
    Decode a 56×7 ori board state into a GameStateSchema-compatible dict.

    The board_ori is in *canonical* form (current player is always "player 0"
    in the matrix).  ``player_perspective`` tells us which real player was
    the current player at this turn.

    Memory layout (for 2-player, n=2):
      Row 0        :  bank  [gems_w, gems_b, gems_g, gems_r, gems_k, gold, round_counter]
      Rows 1–24    :  12 visible cards, 2 rows each (tier0: 1-8, tier1: 9-16, tier2: 17-24)
      Rows 25–30   :  deck info (2 rows per tier)
      Rows 31–33   :  3 nobles
      Rows 34–35   :  players_gems (row 34 = current player, row 35 = opponent)
      Rows 36–41   :  players_nobles (3 per player)
      Rows 42–43   :  players_cards (bonuses[0:5] + points[6])
      Rows 44–55   :  players_reserved (6 rows per player, 2 rows per card, max 3 cards)
    """
    state = board_ori.astype(int)

    # ── Bank ──
    bank_row = state[0]
    bank = [int(bank_row[i]) for i in range(6)]  # [w,b,g,r,k,gold]

    # ── Visible cards ──
    visible_cards: List[List[int]] = [[], [], []]
    for tier in range(3):
        for slot in range(4):
            row_idx = 1 + tier * 8 + slot * 2
            row0 = state[row_idx]
            row1 = state[row_idx + 1]
            card_id = _card_id_from_ori(row0, row1)
            visible_cards[tier].append(card_id)

    # ── Deck counts ──
    deck_counts = []
    for tier in range(3):
        row_idx = 25 + tier * 2
        cnt = int(state[row_idx][:5].sum())
        deck_counts.append(cnt)

    # ── Nobles ──
    nobles = []
    for i in range(3):  # 2-player has 3 nobles
        noble_id = _noble_id_from_ori(state[31 + i])
        if noble_id >= 0:
            nobles.append(noble_id)

    # ── Players ──
    # In canonical form, row index 34 = current player, 35 = opponent.
    # We need to map back to real player indices.
    current_p = player_perspective
    opponent_p = 1 - current_p

    def _decode_player(p_idx: int, is_canonical_first: bool) -> dict:
        """Decode player state from canonical board."""
        # Canonical index: 0 = current player, 1 = opponent
        c_idx = 0 if is_canonical_first else 1

        # Gems
        gems_row = state[34 + c_idx]
        gems = [int(gems_row[i]) for i in range(6)]

        # Card bonuses (accumulated)
        cards_row = state[42 + c_idx]
        bonuses = [int(cards_row[i]) for i in range(5)]
        points = int(cards_row[6])

        # Nobles acquired
        acquired_nobles = []
        for i in range(3):
            noble_id = _noble_id_from_ori(state[36 + c_idx * 3 + i])
            if noble_id >= 0:
                acquired_nobles.append(noble_id)
                points += 3  # each noble = 3 points

        # Reserved cards
        reserved_cards = []
        for slot in range(3):
            row_idx = 44 + c_idx * 6 + slot * 2
            row0 = state[row_idx]
            row1 = state[row_idx + 1]
            card_id = _card_id_from_ori(row0, row1)
            if card_id >= 0:
                reserved_cards.append(card_id)

        return {
            "index": p_idx,
            "gems": gems,
            "bonuses": bonuses,
            "points": points,
            "reserved_cards": reserved_cards,
            "purchased_cards": [],  # Not tracked in board state
            "acquired_nobles": acquired_nobles,
        }

    p_current = _decode_player(current_p, True)
    p_opponent = _decode_player(opponent_p, False)

    players = [None, None]
    players[current_p] = p_current
    players[opponent_p] = p_opponent

    # ── Determine game state ──
    game_over = (turn >= final_turn)
    winner = -1
    if game_over:
        if value_target > 0:
            winner = current_p
        elif value_target < 0:
            winner = opponent_p
        else:
            winner = -2  # draw

    return {
        "board": {
            "bank": bank,
            "visible_cards": visible_cards,
            "deck_counts": deck_counts,
            "nobles": nobles,
            "current_player": current_p,
            "turn": turn,
            "waiting_noble": False,
            "game_over": game_over,
            "winner": winner,
        },
        "players": players,
        "legal_actions": [],  # No actions in replay mode
    }


# ── Pydantic response models ───────────────────────────────────────────

class ReplayFileInfo(BaseModel):
    filename: str
    path: str
    num_examples: int
    size_mb: float

class ReplaySessionInfo(BaseModel):
    session_id: str
    filename: str
    num_games: int
    total_steps: int
    game_lengths: List[int]

class ReplayStepResponse(BaseModel):
    state: dict  # GameState-compatible
    step: int
    total_steps: int
    turn: int
    player: int
    policy_top5: List[dict]  # [{action_idx, probability}]
    value_target: float
    final_turn: int


# ── Endpoints ───────────────────────────────────────────────────────────

@replay_router.get("/files")
async def list_replay_files() -> Dict[str, List[ReplayFileInfo]]:
    """List available .pkl files under alphazero-deepsets/data/."""
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
    data_dir = os.path.join(base_dir, "alphazero-deepsets", "data")

    files = []
    if os.path.isdir(data_dir):
        for pkl_path in sorted(glob.glob(os.path.join(data_dir, "*.pkl"))):
            size_bytes = os.path.getsize(pkl_path)
            # Quick count without fully loading
            try:
                with open(pkl_path, "rb") as f:
                    data = pickle.load(f)
                num_examples = len(data) if isinstance(data, list) else 0
            except Exception:
                num_examples = 0
            files.append(ReplayFileInfo(
                filename=os.path.basename(pkl_path),
                path=pkl_path,
                num_examples=num_examples,
                size_mb=round(size_bytes / 1024 / 1024, 2),
            ))

    return {"files": files}


@replay_router.post("/load")
async def load_replay(path: str) -> ReplaySessionInfo:
    """Load a .pkl file and create a replay session."""
    if not os.path.isfile(path):
        raise HTTPException(status_code=404, detail=f"File not found: {path}")
    if not path.endswith(".pkl"):
        raise HTTPException(status_code=400, detail="Only .pkl files are supported")

    try:
        with open(path, "rb") as f:
            data = pickle.load(f)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Failed to load pkl: {e}")

    if not isinstance(data, list) or len(data) == 0:
        raise HTTPException(status_code=400, detail="Invalid pkl format")

    session_id = str(uuid.uuid4())
    session = ReplaySession(os.path.basename(path), data)
    _sessions[session_id] = session

    return ReplaySessionInfo(
        session_id=session_id,
        filename=session.filename,
        num_games=len(session.games),
        total_steps=session.total_steps,
        game_lengths=[len(g) for g in session.games],
    )


@replay_router.get("/{session_id}/game/{game_idx}/{step}")
async def get_replay_step(
    session_id: str, game_idx: int, step: int
) -> ReplayStepResponse:
    """Get decoded board state for a specific step in a game."""
    if session_id not in _sessions:
        raise HTTPException(status_code=404, detail="Session not found")

    session = _sessions[session_id]
    if game_idx < 0 or game_idx >= len(session.games):
        raise HTTPException(status_code=400, detail="Invalid game index")

    game = session.games[game_idx]
    if step < 0 or step >= len(game):
        raise HTTPException(status_code=400, detail="Invalid step")

    ex = game[step]
    board_ori = ex["board_ori"]
    player = int(ex["player"])
    turn = int(ex["turn"])
    value_target = float(ex.get("value_target", 0.0))
    final_turn = int(ex.get("final_turn", 9999))

    state = _decode_board_ori(board_ori, player, turn, value_target, final_turn)

    # Top-5 policy actions
    policy = ex["policy_406"].astype(float)
    top5_idx = np.argsort(policy)[-5:][::-1]
    policy_top5 = [
        {"action_idx": int(idx), "probability": round(float(policy[idx]), 4)}
        for idx in top5_idx
        if policy[idx] > 0
    ]

    return ReplayStepResponse(
        state=state,
        step=step,
        total_steps=len(game),
        turn=turn,
        player=player,
        policy_top5=policy_top5,
        value_target=value_target,
        final_turn=final_turn,
    )
