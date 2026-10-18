from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

from .. import Game, get_card
from ..gem_colors import GEM_USI_SYMBOLS

# USI gem letters in canonical order:
# W=Diamond(White), U=Sapphire(Blue), G=Emerald(Green), R=Ruby(Red), K=Onyx(Black), D=Gold
GEM_LETTERS: Tuple[str, ...] = tuple(GEM_USI_SYMBOLS)
LETTER_TO_GEM: Dict[str, int] = {c: i for i, c in enumerate(GEM_LETTERS)}

_MOVE_LINE_RE = re.compile(
    r"^(\d+)\.\s+P(\d+)\s+(\S+)(?:\s+\[(\d+)\])?(?:\s+#\s*(.*))?$"
)


def _safe_int(v, default: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        return default


@dataclass
class ParsedUSIMove:
    kind: str
    take: Optional[List[int]] = None
    return_gems: Optional[List[int]] = None
    card_id: Optional[int] = None
    deck_level: Optional[int] = None  # 0-based
    gold_as: Optional[List[int]] = None  # size 5
    pay_gems: Optional[List[int]] = None  # size 6
    noble_id: Optional[int] = None


def board_to_spn(
    board,
    *,
    reveal_hidden_reserved_ids: bool = False,
    require_purchased_card_ids: bool = False,
) -> str:
    """Serialize current board to SPN text."""
    bank = [int(v) for v in board.bank]
    bank_part = f"bank:W{bank[0]}U{bank[1]}G{bank[2]}R{bank[3]}K{bank[4]}D{bank[5]}"

    visible = [[int(c) for c in row] for row in board.visible]

    def _fmt_visible(level_idx: int) -> str:
        vals = ",".join(str(v) if v >= 0 else "-" for v in visible[level_idx])
        return f"L{level_idx + 1}[{vals}]"

    visible_part = "visible:" + "".join(_fmt_visible(i) for i in range(3))

    deck_counts = [len(d) for d in board.decks]
    decks_part = f"decks:{deck_counts[0]},{deck_counts[1]},{deck_counts[2]}"

    nobles = [int(n) for n in board.nobles if int(n) >= 0]
    nobles_part = "nobles:[" + ",".join(str(n) for n in nobles) + "]"

    players_part: List[str] = []
    for p_idx, p in enumerate(board.players):
        gems = [int(v) for v in p.gems]
        bonuses = [int(v) for v in p.bonuses]
        reserved: List[str] = []
        for slot_idx, card_id in enumerate(p.reserved):
            cid = int(card_id)
            if cid < 0:
                continue
            # Public SPN hides deck-reserved identities. Reproducible artifacts
            # may opt into ?C<id>, which preserves both identity and visibility.
            if slot_idx < len(p.reserved_is_hidden) and bool(p.reserved_is_hidden[slot_idx]):
                if reveal_hidden_reserved_ids:
                    reserved.append(f"?C{cid}")
                else:
                    lvl = card_level_from_id(cid)
                    reserved.append(f"?L{lvl}" if lvl > 0 else "?L1")
            else:
                reserved.append(str(cid))
        bought = [str(int(cid)) for cid in p.purchased_cards if int(cid) >= 0]
        unknown_bought_count = int(p.purchased_count) - len(bought)
        if require_purchased_card_ids and unknown_bought_count != 0:
            raise ValueError(
                f"P{p_idx} purchased card IDs are incomplete: "
                f"count={int(p.purchased_count)}, known={len(bought)}"
            )
        bought.extend("_" for _ in range(max(0, unknown_bought_count)))
        acquired_nobles = [
            str(int(noble_id))
            for noble_id in p.acquired_nobles
            if int(noble_id) >= 0
        ]
        players_part.append(
            "P{idx}:gems:W{g0}U{g1}G{g2}R{g3}K{g4}D{g5};"
            "bonuses:W{b0}U{b1}G{b2}R{b3}K{b4};"
            "points:{pts};nobles:[{nobles}];reserved:[{res}];bought:[{bought}]".format(
                idx=p_idx,
                g0=gems[0], g1=gems[1], g2=gems[2], g3=gems[3], g4=gems[4], g5=gems[5],
                b0=bonuses[0], b1=bonuses[1], b2=bonuses[2], b3=bonuses[3], b4=bonuses[4],
                pts=int(p.points),
                nobles=",".join(acquired_nobles),
                res=",".join(reserved),
                bought=",".join(bought),
            )
        )

    return " | ".join(
        [bank_part, visible_part, decks_part, nobles_part, *players_part, str(int(board.current_player))]
    )


def game_to_spn(
    game,
    *,
    reveal_hidden_reserved_ids: bool = False,
    require_purchased_card_ids: bool = False,
) -> str:
    return board_to_spn(
        game.board,
        reveal_hidden_reserved_ids=reveal_hidden_reserved_ids,
        require_purchased_card_ids=require_purchased_card_ids,
    )


def spn_to_game(spn: str, seed: int = 0) -> Game:
    """Build a 2-player game from Splendor Position Notation text."""
    spn = spn.strip()
    if spn.lower().startswith("position "):
        return position_to_game(spn, seed=seed)
    if spn.lower().startswith("startpos"):
        return _startpos_to_game(spn, seed=seed)

    sections = [part.strip() for part in spn.split("|")]
    if len(sections) < 6:
        raise ValueError("SPN must contain bank, visible, decks, nobles, players, and current player")

    current_player: Optional[int] = None
    if re.fullmatch(r"[0-3]", sections[-1]):
        current_player = int(sections[-1])
        sections = sections[:-1]
    else:
        match = re.fullmatch(r"(P\d+:.*)\s+([0-3])", sections[-1])
        if not match:
            raise ValueError("SPN must end with current player id")
        sections[-1] = match.group(1)
        current_player = int(match.group(2))

    if current_player not in (0, 1):
        raise ValueError("csplendor solver supports only 2-player positions")

    bank = _parse_prefixed_counts(sections[0], "bank:", include_gold=True)
    visible = _parse_visible_section(sections[1])
    deck_counts = _parse_deck_counts(sections[2])
    nobles = _parse_nobles_section(sections[3])
    player_sections = sections[4:]
    if len(player_sections) != 2:
        raise ValueError("csplendor solver supports exactly 2 player sections")

    players = [_parse_player_section(player_sections[i], i) for i in range(2)]
    used_cards = _collect_known_cards(visible, players)
    decks = _infer_decks_from_spn(deck_counts, used_cards)

    game = Game(seed=seed)
    board = game.board
    board.bank = bank
    board.visible = visible
    board.decks = decks
    board.nobles = nobles
    board.current_player = current_player

    for idx, pdata in enumerate(players):
        player = board.get_player(idx)
        player.gems = pdata["gems"]
        player.bonuses = pdata["bonuses"]
        player.points = pdata["points"]
        player.reserved = pdata["reserved"]
        player.reserved_is_hidden = pdata["reserved_is_hidden"]
        player.reserved_count = pdata["reserved_count"]
        player.purchased_cards = pdata["bought"]
        player.purchased_count = pdata["purchased_count"]
        player.acquired_nobles = pdata["nobles"]
        board.set_player(idx, player)

    # SPN has no pending-decision field.  A side to move holding more than
    # ten tokens can only arise right after a deck reservation, so the
    # position is the return phase of that reservation.
    if sum(players[current_player]["gems"]) > 10:
        board.waiting_return = True

    return game


def position_to_game(position: str, seed: int = 0) -> Game:
    """Parse a USI `position ... [moves ...]` command or raw SPN/startpos text."""
    text = position.strip()
    if not text:
        raise ValueError("empty position text")
    if text.lower().startswith("position "):
        text = text[len("position "):].strip()

    base, moves = _split_position_moves(text)
    if base.lower().startswith("startpos"):
        game = _startpos_to_game(base, seed=seed)
    else:
        game = spn_to_game(base, seed=seed)

    for move in (ply for text in moves for ply in expand_usi_move(text)):
        idx = find_legal_action_index_by_usi(game, move)
        if idx < 0:
            raise ValueError(f"no legal action matches USI move: {move}")
        action = game.legal_actions[idx]
        if not game.apply(action, False):
            raise RuntimeError(f"engine rejected USI move: {move}")
    return game


def _startpos_to_game(text: str, seed: int) -> Game:
    tokens = text.split()
    if not tokens or tokens[0].lower() != "startpos":
        raise ValueError("start position must start with startpos")
    if len(tokens) > 1 and tokens[1] != "2":
        raise ValueError("csplendor solver supports only startpos 2")
    return Game(seed=seed)


def _split_position_moves(text: str) -> Tuple[str, List[str]]:
    match = re.search(r"\s+moves\s+", text, flags=re.IGNORECASE)
    if not match:
        return text.strip(), []
    base = text[:match.start()].strip()
    moves = [token.strip() for token in text[match.end():].split() if token.strip()]
    return base, moves


def _parse_prefixed_counts(section: str, prefix: str, include_gold: bool) -> List[int]:
    section = section.strip()
    if not section.lower().startswith(prefix):
        raise ValueError(f"expected {prefix} section")
    token = section[len(prefix):].strip().upper()
    allowed = GEM_LETTERS if include_gold else GEM_LETTERS[:5]
    counts = [0] * (6 if include_gold else 5)
    pos = 0
    for match in re.finditer(r"([WUGRKD])(\d+)", token):
        if match.start() != pos:
            raise ValueError(f"invalid gem count token: {token}")
        letter = match.group(1)
        if letter not in allowed:
            raise ValueError(f"gem letter {letter} is not allowed in this section")
        counts[LETTER_TO_GEM[letter]] = int(match.group(2))
        pos = match.end()
    if pos != len(token):
        raise ValueError(f"invalid gem count token: {token}")
    return counts


def _parse_visible_section(section: str) -> List[List[int]]:
    section = section.strip()
    if not section.lower().startswith("visible:"):
        raise ValueError("expected visible section")
    body = section[len("visible:"):].strip()
    levels: List[List[int]] = []
    pos = 0
    pattern = re.compile(r"L([123])\[([^\]]*)\]")
    for match in pattern.finditer(body):
        if match.start() != pos:
            raise ValueError(f"invalid visible section: {section}")
        expected_level = len(levels) + 1
        if int(match.group(1)) != expected_level:
            raise ValueError("visible levels must be L1, L2, L3 in order")
        cards = _parse_card_slots(match.group(2), 4, allow_blank=True)
        for card_id in cards:
            if card_id >= 0 and get_card(card_id).level != expected_level:
                raise ValueError(f"card C{card_id} does not belong to visible L{expected_level}")
        levels.append(cards)
        pos = match.end()
    if pos != len(body) or len(levels) != 3:
        raise ValueError("visible section must contain L1, L2, and L3")
    return levels


def _parse_deck_counts(section: str) -> List[int]:
    section = section.strip()
    if not section.lower().startswith("decks:"):
        raise ValueError("expected decks section")
    parts = [part.strip() for part in section[len("decks:"):].split(",")]
    if len(parts) != 3:
        raise ValueError("decks section must contain 3 counts")
    return [int(part) for part in parts]


def _parse_nobles_section(section: str) -> List[int]:
    section = section.strip()
    if not section.lower().startswith("nobles:"):
        raise ValueError("expected nobles section")
    return _parse_id_list(section[len("nobles:"):], "noble")


def _parse_player_section(section: str, expected_player: int) -> Dict[str, object]:
    section = section.strip()
    prefix = f"P{expected_player}:"
    if not section.startswith(prefix):
        raise ValueError(f"expected P{expected_player} section")
    fields: Dict[str, str] = {}
    for part in section[len(prefix):].split(";"):
        if ":" not in part:
            raise ValueError(f"invalid player field: {part}")
        key, value = part.split(":", 1)
        fields[key.strip()] = value.strip()

    required = {"gems", "bonuses", "points", "reserved", "bought"}
    missing = required - set(fields)
    if missing:
        raise ValueError(f"P{expected_player} missing fields: {sorted(missing)}")

    reserved, reserved_is_hidden = _parse_reserved_section(fields["reserved"])
    if any(card_id < 0 and hidden for card_id, hidden in zip(reserved, reserved_is_hidden)):
        raise ValueError(
            "SPN hidden reserved cards (?Lx) cannot be solved exactly; use explicit card ids"
        )
    if len(reserved) > 3:
        raise ValueError("a player can reserve at most 3 cards")
    reserved_slots = reserved + [-1] * (3 - len(reserved))

    bought_ids, purchased_count, unknown_bought_count = _parse_bought_section(fields["bought"])
    nobles = _parse_nobles_section("nobles:" + fields.get("nobles", "[]"))
    if len(nobles) > 3:
        raise ValueError("a player can acquire at most 3 nobles")

    return {
        "gems": _parse_prefixed_counts("gems:" + fields["gems"], "gems:", include_gold=True),
        "bonuses": _parse_prefixed_counts("bonuses:" + fields["bonuses"], "bonuses:", include_gold=False),
        "points": int(fields["points"]),
        "nobles": nobles,
        "reserved": reserved_slots,
        "reserved_is_hidden": reserved_is_hidden[:3] + [False] * (3 - len(reserved_is_hidden)),
        "reserved_count": len(reserved),
        "bought": bought_ids,
        "purchased_count": purchased_count,
        "unknown_bought_count": unknown_bought_count,
    }


def _parse_card_slots(text: str, expected_len: int, allow_blank: bool) -> List[int]:
    parts = [part.strip() for part in text.split(",")] if text.strip() else []
    if len(parts) != expected_len:
        raise ValueError(f"expected {expected_len} card slots")
    cards: List[int] = []
    for part in parts:
        if allow_blank and part == "-":
            cards.append(-1)
            continue
        card_id = int(part)
        if not 0 <= card_id < 90:
            raise ValueError(f"card id out of range: {card_id}")
        cards.append(card_id)
    return cards


def _parse_id_list(text: str, label: str) -> List[int]:
    text = text.strip()
    match = re.fullmatch(r"\[([^\]]*)\]", text)
    if not match:
        raise ValueError(f"invalid {label} list: {text}")
    body = match.group(1).strip()
    if not body:
        return []
    ids: List[int] = []
    for part in body.split(","):
        item = part.strip()
        if item == "-":
            continue
        value = int(item)
        if label == "card" and not 0 <= value < 90:
            raise ValueError(f"card id out of range: {value}")
        if label == "noble" and not 0 <= value < 12:
            raise ValueError(f"noble id out of range: {value}")
        ids.append(value)
    return ids


def _parse_reserved_section(text: str) -> Tuple[List[int], List[bool]]:
    text = text.strip()
    match = re.fullmatch(r"\[([^\]]*)\]", text)
    if not match:
        raise ValueError(f"invalid reserved list: {text}")
    body = match.group(1).strip()
    if not body:
        return [], []

    reserved: List[int] = []
    hidden: List[bool] = []
    for part in body.split(","):
        item = part.strip()
        exact_hidden = re.fullmatch(r"\?C(\d+)", item, flags=re.IGNORECASE)
        if exact_hidden:
            card_id = int(exact_hidden.group(1))
            if not 0 <= card_id < 90:
                raise ValueError(f"reserved card id out of range: {card_id}")
            reserved.append(card_id)
            hidden.append(True)
            continue
        if re.fullmatch(r"\?L[123]", item, flags=re.IGNORECASE):
            reserved.append(-1)
            hidden.append(True)
            continue
        card_id = int(item)
        if not 0 <= card_id < 90:
            raise ValueError(f"reserved card id out of range: {card_id}")
        reserved.append(card_id)
        hidden.append(False)
    return reserved, hidden


def _parse_bought_section(text: str) -> Tuple[List[int], int, int]:
    text = text.strip()
    match = re.fullmatch(r"\[([^\]]*)\]", text)
    if not match:
        raise ValueError(f"invalid bought list: {text}")
    body = match.group(1).strip()
    if not body:
        return [], 0, 0

    bought_ids: List[int] = []
    unknown_count = 0
    for part in body.split(","):
        item = part.strip()
        if item in {"_", "-"}:
            unknown_count += 1
            continue
        card_id = int(item)
        if not 0 <= card_id < 90:
            raise ValueError(f"bought card id out of range: {card_id}")
        bought_ids.append(card_id)
    return bought_ids, len(bought_ids) + unknown_count, unknown_count


def _collect_known_cards(visible: Sequence[Sequence[int]], players: Sequence[Dict[str, object]]) -> List[int]:
    used: List[int] = []
    for row in visible:
        used.extend(int(card_id) for card_id in row if int(card_id) >= 0)
    for pdata in players:
        used.extend(int(card_id) for card_id in pdata["reserved"] if int(card_id) >= 0)
        used.extend(int(card_id) for card_id in pdata["bought"])
    if len(used) != len(set(used)):
        raise ValueError("SPN contains duplicate card ids")
    return used


def _infer_decks_from_spn(
    deck_counts: Sequence[int],
    used_cards: Sequence[int],
) -> List[List[int]]:
    used = set(int(card_id) for card_id in used_cards)
    decks: List[List[int]] = []
    for level in range(1, 4):
        cards = [
            card_id for card_id in range(90)
            if get_card(card_id).level == level and card_id not in used
        ]
        expected_count = int(deck_counts[level - 1])
        if len(cards) != expected_count:
            raise ValueError(
                f"decks L{level} count mismatch: SPN says {expected_count}, "
                f"but known card ids imply {len(cards)}"
            )
        decks.append(cards)
    return decks


# Historical import path. New code uses the focused modules below; rebinding
# keeps every public callable and return DTO available from ``usi_kifu``.
# isort: off
from .kifu_codec import (  # noqa: E402,F811
    build_kifu_text as build_kifu_text,
    now_iso as now_iso,
    parse_kifu_text as parse_kifu_text,
)
from .usi_parser import parse_usi_move as parse_usi_move  # noqa: E402,F811
from .usi_resolver import (  # noqa: E402,F811
    expand_usi_move as expand_usi_move,
    find_legal_action_index_by_usi as find_legal_action_index_by_usi,
)
from .usi_serializer import (  # noqa: E402,F811
    action_to_usi as action_to_usi,
    card_level_from_id as card_level_from_id,
)
from .usi_types import ParsedUSIMove as ParsedUSIMove  # noqa: E402,F811
# isort: on
