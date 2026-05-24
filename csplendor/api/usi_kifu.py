from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import re
from typing import Dict, List, Optional, Sequence, Tuple

from .. import ActionType as CoreActionType
from .. import get_card

# USI gem letters in canonical order:
# W=Diamond(White), U=Sapphire(Blue), G=Emerald(Green), R=Ruby(Red), K=Onyx(Black), D=Gold
GEM_LETTERS: Tuple[str, ...] = ("W", "U", "G", "R", "K", "D")
LETTER_TO_GEM: Dict[str, int] = {c: i for i, c in enumerate(GEM_LETTERS)}

_MOVE_LINE_RE = re.compile(
    r"^(\d+)\.\s+P(\d+)\s+(\S+)(?:\s+\[(\d+)\])?(?:\s+#\s*(.*))?$"
)


def _safe_int(v, default: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        return default


def _counts_to_letters(counts: Sequence[int], include_gold: bool = True) -> str:
    out: List[str] = []
    end = 6 if include_gold else 5
    for i in range(end):
        cnt = _safe_int(counts[i]) if i < len(counts) else 0
        if cnt > 0:
            out.append(GEM_LETTERS[i] * cnt)
    return "".join(out)


def _letters_to_counts(token: str, allow_gold: bool = True) -> List[int]:
    counts = [0, 0, 0, 0, 0, 0]
    for ch in token.strip().upper():
        if ch not in LETTER_TO_GEM:
            raise ValueError(f"invalid gem letter: {ch}")
        idx = LETTER_TO_GEM[ch]
        if idx == 5 and not allow_gold:
            raise ValueError("gold is not allowed in this token")
        counts[idx] += 1
    return counts


def _gold_as_to_token(gold_as: Sequence[int]) -> str:
    parts: List[str] = []
    for i in range(5):
        cnt = _safe_int(gold_as[i]) if i < len(gold_as) else 0
        if cnt > 0:
            parts.append(f"{GEM_LETTERS[i]}{cnt}")
    return "".join(parts)


def _token_to_gold_as(token: str) -> List[int]:
    token = token.strip().upper()
    out = [0, 0, 0, 0, 0]
    for letter, num in re.findall(r"([WUGRK])(\d+)", token):
        out[LETTER_TO_GEM[letter]] += int(num)
    if "".join(f"{GEM_LETTERS[i]}{out[i]}" for i in range(5) if out[i] > 0) != token:
        raise ValueError(f"invalid gold assignment token: {token}")
    return out


def _pay_counts_to_token(pay: Sequence[int]) -> str:
    vals = [int(pay[i]) if i < len(pay) else 0 for i in range(6)]
    return (
        f"W{vals[0]}U{vals[1]}G{vals[2]}R{vals[3]}K{vals[4]}D{vals[5]}"
    )


def _token_to_pay_counts(token: str) -> List[int]:
    token = token.strip().upper()
    if not token:
        raise ValueError("empty pay token")
    out = [0, 0, 0, 0, 0, 0]
    i = 0
    while i < len(token):
        letter = token[i]
        if letter not in LETTER_TO_GEM:
            raise ValueError(f"invalid pay token letter: {letter}")
        i += 1
        j = i
        while j < len(token) and token[j].isdigit():
            j += 1
        if j == i:
            raise ValueError(f"missing pay token count after {letter}")
        out[LETTER_TO_GEM[letter]] += int(token[i:j])
        i = j
    return out


def _compute_purchase_payment(action, game) -> Optional[List[int]]:
    if int(action.type) != int(CoreActionType.PURCHASE):
        return None
    if game is None:
        return None
    try:
        card = get_card(int(action.card_id))
        player = game.board.players[game.board.current_player]
        bonuses = [int(v) for v in player.bonuses]
    except Exception:
        return None

    gold_as = [int(v) for v in action.gold_as[:5]]
    pay = [0, 0, 0, 0, 0, 0]
    for i in range(5):
        effective = max(0, int(card.cost[i]) - bonuses[i])
        pay[i] = max(0, effective - gold_as[i])
    pay[5] = sum(gold_as)
    return pay


def card_level_from_id(card_id: int) -> int:
    if 0 <= card_id <= 39:
        return 1
    if 40 <= card_id <= 69:
        return 2
    if 70 <= card_id <= 89:
        return 3
    return 0


def action_to_usi(action, game=None) -> str:
    """Convert csplendor.Action to USI move notation string."""
    if action.type in (CoreActionType.TAKE_DIFFERENT, CoreActionType.TAKE_SAME):
        take = _counts_to_letters(list(action.take), include_gold=True)
        if not take:
            return "pass"
        ret = _counts_to_letters(list(action.return_gems), include_gold=True)
        return f"take:{take}" + (f"/return:{ret}" if ret else "")

    if action.type == CoreActionType.RESERVE_VISIBLE:
        base = f"reserve:C{_safe_int(action.card_id)}"
        ret = _counts_to_letters(list(action.return_gems), include_gold=True)
        return base + (f"/return:{ret}" if ret else "")

    if action.type == CoreActionType.RESERVE_DECK:
        base = f"reserve:L{_safe_int(action.deck_level) + 1}"
        ret = _counts_to_letters(list(action.return_gems), include_gold=True)
        return base + (f"/return:{ret}" if ret else "")

    if action.type == CoreActionType.PURCHASE:
        base = f"buy:C{_safe_int(action.card_id)}"
        pay = _compute_purchase_payment(action, game)
        if pay is not None:
            return base + f"/pay:{_pay_counts_to_token(pay)}"
        gold = _gold_as_to_token(list(action.gold_as))
        return base + (f"/gold:{gold}" if gold else "")

    if action.type == CoreActionType.VISIT_NOBLE:
        return f"noble:N{_safe_int(action.noble_choice)}"

    return "pass"


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


def parse_usi_move(text: str) -> ParsedUSIMove:
    s = text.strip()
    if s.lower() == "pass":
        return ParsedUSIMove(kind="pass")

    m = re.fullmatch(r"take:([WUGRKD]+)(?:/return:([WUGRKD]+))?", s, flags=re.IGNORECASE)
    if m:
        take = _letters_to_counts(m.group(1), allow_gold=False)
        ret = _letters_to_counts(m.group(2), allow_gold=True) if m.group(2) else None
        return ParsedUSIMove(kind="take", take=take, return_gems=ret)

    m = re.fullmatch(r"reserve:C(\d+)(?:/return:([WUGRKD]+))?", s, flags=re.IGNORECASE)
    if m:
        ret = _letters_to_counts(m.group(2), allow_gold=True) if m.group(2) else None
        return ParsedUSIMove(kind="reserve_visible", card_id=int(m.group(1)), return_gems=ret)

    m = re.fullmatch(r"reserve:L([123])(?:/return:([WUGRKD]+))?", s, flags=re.IGNORECASE)
    if m:
        ret = _letters_to_counts(m.group(2), allow_gold=True) if m.group(2) else None
        return ParsedUSIMove(kind="reserve_deck", deck_level=int(m.group(1)) - 1, return_gems=ret)

    m = re.fullmatch(r"buy:C(\d+)(.*)", s, flags=re.IGNORECASE)
    if m:
        card_id = int(m.group(1))
        tail = m.group(2) or ""
        noble_id: Optional[int] = None
        gold: Optional[List[int]] = None
        pay_gems: Optional[List[int]] = None

        noble_match = re.search(r"\s+noble:N(\d+)$", tail, flags=re.IGNORECASE)
        if noble_match:
            noble_id = int(noble_match.group(1))
            tail = tail[: noble_match.start()]

        tail = tail.strip()
        if tail:
            parts = [p for p in tail.split("/") if p]
            for part in parts:
                p = part.strip()
                low = p.lower()
                if low.startswith("gold:"):
                    if gold is not None:
                        raise ValueError(f"duplicate gold token in USI move: {text}")
                    gold = _token_to_gold_as(p.split(":", 1)[1])
                elif low.startswith("pay:"):
                    if pay_gems is not None:
                        raise ValueError(f"duplicate pay token in USI move: {text}")
                    pay_gems = _token_to_pay_counts(p.split(":", 1)[1])
                else:
                    raise ValueError(f"invalid buy suffix token: {p}")

        return ParsedUSIMove(
            kind="buy",
            card_id=card_id,
            gold_as=gold,
            pay_gems=pay_gems,
            noble_id=noble_id,
        )

    m = re.fullmatch(r"noble:N(\d+)", s, flags=re.IGNORECASE)
    if m:
        return ParsedUSIMove(kind="noble", noble_id=int(m.group(1)))

    raise ValueError(f"invalid USI move: {text}")


def _returns_match(expected: Optional[List[int]], actual: Sequence[int]) -> bool:
    if expected is None:
        return sum(_safe_int(v) for v in actual) == 0
    return [int(v) for v in actual[:6]] == expected[:6]


def find_legal_action_index_by_usi(game, usi_move: str) -> int:
    """Resolve a USI move string to legal action index in current game state."""
    usi_move = usi_move.strip()
    legal_actions = game.legal_actions

    # Fast path: exact canonical serialization match.
    for i, action in enumerate(legal_actions):
        if action_to_usi(action, game=game) == usi_move:
            return i

    parsed = parse_usi_move(usi_move)

    if parsed.kind == "pass":
        return -1 if len(legal_actions) == 0 else -1

    if parsed.kind == "take":
        for i, action in enumerate(legal_actions):
            if action.type not in (CoreActionType.TAKE_DIFFERENT, CoreActionType.TAKE_SAME):
                continue
            if [int(v) for v in action.take[:6]] != parsed.take:
                continue
            if _returns_match(parsed.return_gems, action.return_gems):
                return i
        raise ValueError(f"no legal take action matches USI move: {usi_move}")

    if parsed.kind == "reserve_visible":
        for i, action in enumerate(legal_actions):
            if action.type != CoreActionType.RESERVE_VISIBLE:
                continue
            if int(action.card_id) != int(parsed.card_id):
                continue
            if _returns_match(parsed.return_gems, action.return_gems):
                return i
        raise ValueError(f"no legal reserve-visible action matches USI move: {usi_move}")

    if parsed.kind == "reserve_deck":
        for i, action in enumerate(legal_actions):
            if action.type != CoreActionType.RESERVE_DECK:
                continue
            if int(action.deck_level) != int(parsed.deck_level):
                continue
            if _returns_match(parsed.return_gems, action.return_gems):
                return i
        raise ValueError(f"no legal reserve-deck action matches USI move: {usi_move}")

    if parsed.kind == "buy":
        candidates: List[Tuple[int, int, int]] = []
        for i, action in enumerate(legal_actions):
            if action.type != CoreActionType.PURCHASE:
                continue
            if int(action.card_id) != int(parsed.card_id):
                continue
            gold_as = [int(v) for v in action.gold_as[:5]]
            if parsed.gold_as is not None and gold_as != parsed.gold_as:
                continue
            payment = _compute_purchase_payment(action, game)
            if parsed.pay_gems is not None:
                if payment is None or payment != parsed.pay_gems:
                    continue
            # Tie-break (if gold omitted): minimal gold usage then minimal return.
            candidates.append((i, sum(gold_as), sum(int(v) for v in action.return_gems[:6])))
        if not candidates:
            raise ValueError(f"no legal buy action matches USI move: {usi_move}")
        candidates.sort(key=lambda x: (x[1], x[2], x[0]))
        return candidates[0][0]

    if parsed.kind == "noble":
        for i, action in enumerate(legal_actions):
            if action.type != CoreActionType.VISIT_NOBLE:
                continue
            if int(action.noble_choice) == int(parsed.noble_id):
                return i
        raise ValueError(f"no legal noble action matches USI move: {usi_move}")

    raise ValueError(f"unsupported USI move kind: {parsed.kind}")


def board_to_spn(board) -> str:
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
            # Keep hidden cards as ?Lx when available.
            if slot_idx < len(p.reserved_is_hidden) and bool(p.reserved_is_hidden[slot_idx]):
                lvl = card_level_from_id(cid)
                reserved.append(f"?L{lvl}" if lvl > 0 else "?L1")
            else:
                reserved.append(str(cid))
        bought = [str(int(cid)) for cid in p.purchased_cards if int(cid) >= 0]
        players_part.append(
            "P{idx}:gems:W{g0}U{g1}G{g2}R{g3}K{g4}D{g5};"
            "bonuses:W{b0}U{b1}G{b2}R{b3}K{b4};"
            "points:{pts};reserved:[{res}];bought:[{bought}]".format(
                idx=p_idx,
                g0=gems[0], g1=gems[1], g2=gems[2], g3=gems[3], g4=gems[4], g5=gems[5],
                b0=bonuses[0], b1=bonuses[1], b2=bonuses[2], b3=bonuses[3], b4=bonuses[4],
                pts=int(p.points),
                res=",".join(reserved),
                bought=",".join(bought),
            )
        )

    return " | ".join(
        [bank_part, visible_part, decks_part, nobles_part, *players_part, str(int(board.current_player))]
    )


def game_to_spn(game) -> str:
    return board_to_spn(game.board)


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def build_kifu_text(
    headers: Dict[str, str],
    position: str,
    moves: Sequence[Dict[str, object]],
    result: str,
    final_scores: Optional[Sequence[int]] = None,
    total_turns: Optional[int] = None,
) -> str:
    ordered = ["Format", "Players", "Player0", "Player1", "Date", "Event", "Round", "Seed"]
    lines: List[str] = []
    for key in ordered:
        if key in headers and headers[key] is not None:
            lines.append(f"{key}: {headers[key]}")
    for key, value in headers.items():
        if key in ordered:
            continue
        if value is None:
            continue
        lines.append(f"{key}: {value}")

    lines.append("")
    lines.append(f"Position: {position}")
    lines.append("")

    for i, mv in enumerate(moves, start=1):
        player = _safe_int(mv.get("player"), 0)
        usi = str(mv.get("usi", "pass"))
        time_ms = mv.get("time_ms")
        comment = mv.get("comment")
        line = f"{i}. P{player} {usi}"
        if time_ms is not None:
            line += f" [{_safe_int(time_ms, 0)}]"
        if comment:
            line += f" # {comment}"
        lines.append(line)

    lines.append("")
    lines.append(f"Result: {result}")
    if final_scores is not None:
        parts = [f"P{i}={_safe_int(s)}" for i, s in enumerate(final_scores)]
        lines.append("FinalScores: " + " ".join(parts))
    if total_turns is not None:
        lines.append(f"TotalTurns: {_safe_int(total_turns)}")
    lines.append("")
    return "\n".join(lines)


def parse_kifu_text(text: str) -> Dict[str, object]:
    headers: Dict[str, str] = {}
    position = ""
    moves: List[Dict[str, object]] = []
    result = ""
    final_scores: Optional[List[int]] = None
    total_turns: Optional[int] = None

    mode = "header"
    for raw in text.splitlines():
        line = raw.rstrip("\n")
        stripped = line.strip()
        if not stripped:
            if mode == "header":
                mode = "position"
            elif mode == "position":
                mode = "moves"
            elif mode == "moves":
                mode = "result"
            continue
        if stripped.startswith("#"):
            continue

        if mode == "header":
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip()] = v.strip()
            continue

        if mode == "position":
            if stripped.startswith("Position:"):
                position = stripped[len("Position:"):].strip()
            continue

        if mode == "moves":
            m = _MOVE_LINE_RE.match(stripped)
            if not m:
                continue
            _, p, usi, tms, comment = m.groups()
            entry: Dict[str, object] = {"player": int(p), "usi": usi}
            if tms is not None:
                entry["time_ms"] = int(tms)
            if comment:
                entry["comment"] = comment
            moves.append(entry)
            continue

        if mode == "result":
            if stripped.startswith("Result:"):
                result = stripped[len("Result:"):].strip()
            elif stripped.startswith("FinalScores:"):
                fs = stripped[len("FinalScores:"):].strip()
                vals = []
                for token in fs.split():
                    if "=" not in token:
                        continue
                    _, v = token.split("=", 1)
                    vals.append(_safe_int(v))
                if vals:
                    final_scores = vals
            elif stripped.startswith("TotalTurns:"):
                total_turns = _safe_int(stripped[len("TotalTurns:"):].strip())

    return {
        "headers": headers,
        "position": position,
        "moves": moves,
        "result": result,
        "final_scores": final_scores,
        "total_turns": total_turns,
    }
