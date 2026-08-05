"""KIFU text serialization and parsing without web or engine dependencies."""

import re
from datetime import datetime, timezone
from typing import Dict, List, Optional, Sequence

from .usi_tokens import safe_int

_MOVE_LINE_RE = re.compile(
    r"^(\d+)\.\s+P(\d+)\s+(\S+)(?:\s+\[(\d+)\])?"
    r"(?:\s+#\s*(.*))?$"
)


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
    ordered = [
        "Format",
        "Players",
        "Player0",
        "Player1",
        "Date",
        "Event",
        "Round",
        "Seed",
    ]
    lines: List[str] = []
    for key in ordered:
        if key in headers and headers[key] is not None:
            lines.append(f"{key}: {headers[key]}")
    for key, value in headers.items():
        if key not in ordered and value is not None:
            lines.append(f"{key}: {value}")
    lines.extend(("", f"Position: {position}", ""))
    for index, move in enumerate(moves, start=1):
        player = safe_int(move.get("player"), 0)
        usi = str(move.get("usi", "pass"))
        line = f"{index}. P{player} {usi}"
        if move.get("time_ms") is not None:
            line += f" [{safe_int(move.get('time_ms'), 0)}]"
        if move.get("comment"):
            line += f" # {move['comment']}"
        lines.append(line)
    lines.extend(("", f"Result: {result}"))
    if final_scores is not None:
        parts = [
            f"P{index}={safe_int(score)}"
            for index, score in enumerate(final_scores)
        ]
        lines.append("FinalScores: " + " ".join(parts))
    if total_turns is not None:
        lines.append(f"TotalTurns: {safe_int(total_turns)}")
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
            mode = {
                "header": "position",
                "position": "moves",
                "moves": "result",
            }.get(mode, mode)
            continue
        if stripped.startswith("#"):
            continue
        if mode == "header":
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip()] = value.strip()
            continue
        if mode == "position":
            if stripped.startswith("Position:"):
                position = stripped[len("Position:") :].strip()
            continue
        if mode == "moves":
            match = _MOVE_LINE_RE.match(stripped)
            if match:
                _, player, usi, time_ms, comment = match.groups()
                entry: Dict[str, object] = {
                    "player": int(player),
                    "usi": usi,
                }
                if time_ms is not None:
                    entry["time_ms"] = int(time_ms)
                if comment:
                    entry["comment"] = comment
                moves.append(entry)
            continue
        if stripped.startswith("Result:"):
            result = stripped[len("Result:") :].strip()
        elif stripped.startswith("FinalScores:"):
            values = []
            for token in stripped[len("FinalScores:") :].strip().split():
                if "=" in token:
                    values.append(safe_int(token.split("=", 1)[1]))
            if values:
                final_scores = values
        elif stripped.startswith("TotalTurns:"):
            total_turns = safe_int(stripped[len("TotalTurns:") :].strip())
    return {
        "headers": headers,
        "position": position,
        "moves": moves,
        "result": result,
        "final_scores": final_scores,
        "total_turns": total_turns,
    }
