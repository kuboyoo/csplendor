"""Render development cards and noble tiles as SVG images.

The data comes from the engine (``csplendor.get_all_cards`` /
``get_all_nobles``), so the images cannot drift from ``card_data.h`` and
``noble_data.h``. Run after changing the catalogue:

    python scripts/render_card_assets.py            # writes assets/
    python scripts/render_card_assets.py --check    # fails if assets/ is stale

Outputs:
    assets/cards/card_XX.svg       one development card per file
    assets/cards/level{1,2,3}.svg  every card of a level, one row per bonus colour
    assets/nobles/noble_XX.svg     one noble tile per file
    assets/nobles/nobles.svg       all noble tiles
    assets/play/cards/card_XX.svg  play edition: no ids, larger numerals
    assets/play/nobles/noble_XX.svg
    assets/play/decks/deck_l{1,2,3}.svg  face-down deck of a level
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Callable, Dict, List, Sequence, Tuple
from xml.sax.saxutils import escape

import csplendor as cs

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"

# Engine colour order: White, Blue, Green, Red, Black (Gold has no cards).
COLOR_NAMES = ("white", "blue", "green", "red", "black")
COLOR_LABELS_JA = ("白", "青", "緑", "赤", "黒")
GEM = {
    # fill, edge, highlight, text on gem
    "white": ("#F4F1EA", "#8D8778", "#FFFFFF", "#3A3730"),
    "blue": ("#1F5FBF", "#123A75", "#6E9BE0", "#FFFFFF"),
    "green": ("#1E8E4E", "#105A30", "#5CC189", "#FFFFFF"),
    "red": ("#C9302C", "#7D1714", "#EE7A76", "#FFFFFF"),
    "black": ("#2B2B2E", "#000000", "#6C6C72", "#FFFFFF"),
}
# Card face tint per bonus colour: top and bottom of the background gradient.
TINT = {
    "white": ("#FBFAF6", "#DCD8CC"),
    "blue": ("#E3ECFA", "#9DBBE8"),
    "green": ("#E1F4E8", "#97D1AE"),
    "red": ("#FBE3E1", "#E59A96"),
    "black": ("#E6E6E8", "#9A9AA0"),
}
# Points ribbon on development cards: a light tint of the bonus colour.
RIBBON_TINT = {
    "white": "#E4E0D6",
    "blue": "#8FB2E6",
    "green": "#8CCBA5",
    "red": "#E8938F",
    "black": "#8E8E95",
}
LEVEL_FRAME = {1: "#3F8F4A", 2: "#C9A227", 3: "#2F63A8"}
FONT = "font-family=\"'Helvetica Neue', Helvetica, Arial, sans-serif\""

# Portrait style per noble tile, by engine noble id. The original tiles depict
# historical people; only their gender is used here (king or queen artwork)
# and no names are rendered.
NOBLE_PORTRAIT = {
    0: "queen", 1: "king", 2: "king", 3: "queen", 4: "king", 5: "queen",
    6: "king", 7: "queen", 8: "king", 9: "queen", 10: "queen", 11: "queen",
}
ROBES = ("#7A1F2B", "#1F3F7A", "#2E5E3A", "#5B2A6E", "#8A4B12", "#23395B",
         "#6B1E4A", "#2F4F4F", "#7A3B1F", "#3E2F6B", "#4A5A1E", "#5A1F1F")
HAIR = ("#5A3A22", "#2E2620", "#8C4A2F", "#C9A04E", "#6E5A48", "#3B2A1E")
SKIN, SKIN_EDGE = "#F2CDA9", "#B98A63"
GOLD, GOLD_EDGE = "#D9A62E", "#7A5418"

E3 = cs.ActionEncoderV3


def v3_purchase_range(card_id: int) -> Tuple[int, int]:
    """First and last V3 policy index of the purchase actions of a card."""
    first = int(E3.OFFSET_PURCHASE) + int(E3.get_card_payment_offset(card_id))
    return first, first + int(E3.get_card_pattern_count(card_id)) - 1


def v3_noble_index(noble_id: int) -> int:
    return int(E3.OFFSET_VISIT_NOBLE) + noble_id


CARD_W, CARD_H = 140, 196
NOBLE = 140
GAP = 10


def gem_icon(cx: float, cy: float, size: float, color: str) -> str:
    """A brilliant-cut gem seen from above (crown facets and a table)."""
    fill, edge, light, _ = GEM[color]
    s = size / 2
    outer = [
        (cx - s * 0.62, cy - s * 0.78), (cx + s * 0.62, cy - s * 0.78),
        (cx + s, cy - s * 0.18), (cx, cy + s), (cx - s, cy - s * 0.18),
    ]
    table = [
        (cx - s * 0.36, cy - s * 0.78), (cx + s * 0.36, cy - s * 0.78),
        (cx + s * 0.5, cy - s * 0.18), (cx - s * 0.5, cy - s * 0.18),
    ]
    fmt = lambda pts: " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)  # noqa: E731
    return (
        f'<polygon points="{fmt(outer)}" fill="{fill}" stroke="{edge}" stroke-width="1.6" stroke-linejoin="round"/>'
        f'<polygon points="{fmt(table)}" fill="{light}" opacity="0.55"/>'
        f'<polyline points="{fmt([outer[4], table[3], (cx, cy + s)])}" fill="none" stroke="{edge}" stroke-width="0.9" opacity="0.6"/>'
        f'<polyline points="{fmt([outer[2], table[2], (cx, cy + s)])}" fill="none" stroke="{edge}" stroke-width="0.9" opacity="0.6"/>'
    )


def numeral(x: float, y: float, size: float, weight: int, fill: str, value: int,
            outline: str = "") -> str:
    """A centred number; with ``outline`` the digits get a stroke in that colour."""
    return (
        f'<text x="{x}" y="{y}" text-anchor="middle" font-size="{size}" font-weight="{weight}" '
        f'fill="{fill}"'
        + (f' stroke="{outline}" stroke-width="3" paint-order="stroke" stroke-linejoin="round"' if outline else "")
        + f' {FONT}>{value}</text>'
    )


def cost_token(cx: float, cy: float, color: str, amount: int, play: bool = False) -> str:
    fill, edge, light, text = GEM[color]
    r, size, weight, dy, hl = (16, 23, 800, 7.8, 4.8) if play else (13, 16, 700, 5.5, 4)
    return (
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{fill}" stroke="{edge}" stroke-width="1.6"/>'
        f'<circle cx="{cx - hl}" cy="{cy - hl}" r="{hl}" fill="{light}" opacity="0.45"/>'
        + numeral(cx, cy + dy, size, weight, text, amount, edge if play else "")
    )


def requirement_tile(x: float, y: float, color: str, amount: int, play: bool = False) -> str:
    fill, edge, light, text = GEM[color]
    side, size, weight, dy, hw, hh = (34, 24, 800, 25.2, 11, 7) if play else (24, 15, 700, 17.5, 8, 5)
    return (
        f'<rect x="{x}" y="{y}" width="{side}" height="{side}" rx="4" fill="{fill}" stroke="{edge}" stroke-width="1.6"/>'
        f'<rect x="{x + 3}" y="{y + 3}" width="{hw}" height="{hh}" rx="2" fill="{light}" opacity="0.45"/>'
        + numeral(x + side / 2, y + dy, size, weight, text, amount, edge if play else "")
    )


def points_label(points: int, fill: str, edge: str, text: str,
                 outline: str = "", play: bool = False) -> str:
    """Prestige points as a ribbon in the top-left corner. Cards and noble
    tiles share the shape so equal values read the same; colours follow the
    tile (the card's bonus colour, or the noble tile's parchment)."""
    # paint-order hides the inner half of the outline, so a 3px stroke
    # shows the same 1.5px width as the ribbon's edge
    if play:
        return (
            f'<path d="M5,1.5 H49 V55 L27,45 L5,55 Z" fill="{fill}" stroke="{edge}" stroke-width="1.5"/>'
            + numeral(27, 37, 36, 800, text, points, outline)
        )
    return (
        f'<path d="M8,1.5 H36 V40 L22,33 L8,40 Z" fill="{fill}" stroke="{edge}" stroke-width="1.5"/>'
        + numeral(22, 27, 22, 800, text, points, outline)
    )


def crown(cx: float, base: float, half: float, height: float, peaks: int) -> str:
    """A gold crown whose band sits at y=base."""
    xs = [cx - half + i * (2 * half) / (peaks * 2) for i in range(peaks * 2 + 1)]
    points = [(xs[0], base)]
    for i, x in enumerate(xs):
        points.append((x, base - height if i % 2 == 0 else base - height * 0.45))
    points.append((xs[-1], base))
    pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
    jewels = "".join(
        f'<circle cx="{xs[i]:.1f}" cy="{base - height:.1f}" r="1.9" fill="{color}" '
        f'stroke="{GOLD_EDGE}" stroke-width="0.6"/>'
        for i, color in zip(range(0, len(xs), 2), ("#1F5FBF", "#C9302C", "#1E8E4E", "#C9302C", "#1F5FBF"))
    )
    return (
        f'<polygon points="{pts}" fill="{GOLD}" stroke="{GOLD_EDGE}" stroke-width="1.2" stroke-linejoin="round"/>'
        f'<rect x="{cx - half:.1f}" y="{base:.1f}" width="{2 * half:.1f}" height="4.5" rx="1" '
        f'fill="#C08A1E" stroke="{GOLD_EDGE}" stroke-width="1"/>{jewels}'
    )


def ermine(cx: float, cy: float, rx: float) -> str:
    dots = "".join(
        f'<path d="M{cx + dx:.1f},{cy + dy:.1f} l1.2,2.6 l-2.4,0 Z" fill="#1F1B16"/>'
        for dx, dy in ((-rx * 0.62, -1), (-rx * 0.22, 2), (rx * 0.22, 2), (rx * 0.62, -1))
    )
    return (
        f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="7.5" fill="#FBF8F1" stroke="#CFC6B5" stroke-width="1"/>'
        + dots
    )


def portrait(cx: float, top: float, kind: str, robe: str, hair: str) -> str:
    """A stylised king or queen bust in the noble tile's portrait area."""
    head_y = top + 54
    parts = []
    if kind == "queen":
        # long hair behind the head, falling to the shoulders
        parts.append(
            f'<path d="M{cx - 17},{head_y} Q{cx - 21},{head_y + 26} {cx - 13},{head_y + 33} '
            f'L{cx + 13},{head_y + 33} Q{cx + 21},{head_y + 26} {cx + 17},{head_y} '
            f'Q{cx + 16},{head_y - 18} {cx},{head_y - 18} Q{cx - 16},{head_y - 18} {cx - 17},{head_y} Z" '
            f'fill="{hair}"/>'
        )
    # robe and shoulders
    parts.append(
        f'<path d="M{cx - 38},{top + 108} Q{cx - 37},{top + 80} {cx},{top + 75} '
        f'Q{cx + 37},{top + 80} {cx + 38},{top + 108} Z" fill="{robe}" stroke="#2A1C10" stroke-width="1"/>'
    )
    parts.append(
        f'<rect x="{cx - 5}" y="{head_y + 10}" width="10" height="12" fill="{SKIN}" stroke="{SKIN_EDGE}" stroke-width="0.8"/>'
    )
    parts.append(ermine(cx, top + 80, 24))
    if kind == "queen":
        parts.append("".join(
            f'<circle cx="{cx + dx:.1f}" cy="{top + 84 + abs(dx) * -0.12:.1f}" r="1.5" fill="#FFFFFF" stroke="#B9B2A4" stroke-width="0.5"/>'
            for dx in (-12, -8, -4, 0, 4, 8, 12)
        ))
    # head
    parts.append(f'<circle cx="{cx}" cy="{head_y}" r="14" fill="{SKIN}" stroke="{SKIN_EDGE}" stroke-width="1"/>')
    if kind == "queen":
        # fringe and a soft face
        parts.append(
            f'<path d="M{cx - 14},{head_y - 2} Q{cx - 8},{head_y - 14} {cx},{head_y - 11} '
            f'Q{cx + 8},{head_y - 14} {cx + 14},{head_y - 2} Q{cx + 12},{head_y - 15} {cx},{head_y - 15} '
            f'Q{cx - 12},{head_y - 15} {cx - 14},{head_y - 2} Z" fill="{hair}"/>'
        )
        parts.append(
            f'<circle cx="{cx - 8}" cy="{head_y + 5}" r="2.6" fill="#E8A3A0" opacity="0.6"/>'
            f'<circle cx="{cx + 8}" cy="{head_y + 5}" r="2.6" fill="#E8A3A0" opacity="0.6"/>'
            f'<ellipse cx="{cx}" cy="{head_y + 7.5}" rx="3" ry="1.5" fill="#B8423F"/>'
        )
    else:
        # hair, moustache and beard
        parts.append(
            f'<path d="M{cx - 14},{head_y - 1} Q{cx - 13},{head_y - 15} {cx},{head_y - 14} '
            f'Q{cx + 13},{head_y - 15} {cx + 14},{head_y - 1} Q{cx + 9},{head_y - 9} {cx},{head_y - 9} '
            f'Q{cx - 9},{head_y - 9} {cx - 14},{head_y - 1} Z" fill="{hair}"/>'
        )
        parts.append(
            f'<path d="M{cx - 13.5},{head_y + 2} Q{cx - 12},{head_y + 22} {cx},{head_y + 24} '
            f'Q{cx + 12},{head_y + 22} {cx + 13.5},{head_y + 2} Q{cx + 7},{head_y + 12} {cx},{head_y + 11} '
            f'Q{cx - 7},{head_y + 12} {cx - 13.5},{head_y + 2} Z" fill="{hair}"/>'
            f'<path d="M{cx - 7},{head_y + 7} Q{cx},{head_y + 4} {cx + 7},{head_y + 7} Q{cx},{head_y + 9} {cx - 7},{head_y + 7} Z" '
            f'fill="{hair}"/>'
        )
    parts.append(
        f'<circle cx="{cx - 5}" cy="{head_y - 1}" r="1.6" fill="#2A1C10"/>'
        f'<circle cx="{cx + 5}" cy="{head_y - 1}" r="1.6" fill="#2A1C10"/>'
    )
    if kind == "queen":
        parts.append(crown(cx, head_y - 15, 9, 9, 2))
    else:
        parts.append(crown(cx, head_y - 13, 14, 14, 2))
    return "".join(parts)


def card_body(card, uid: str, play: bool = False) -> str:
    color = COLOR_NAMES[int(card.bonus)]
    top, bottom = TINT[color]
    frame = LEVEL_FRAME[int(card.level)]
    band = 62 if play else 50
    parts = [
        f'<defs><linearGradient id="bg{uid}" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/>'
        f"</linearGradient></defs>",
        f'<rect x="1.5" y="1.5" width="{CARD_W - 3}" height="{CARD_H - 3}" rx="11" '
        f'fill="url(#bg{uid})" stroke="{frame}" stroke-width="3"/>',
        # faint watermark of the bonus gem in the art area
        f'<g opacity="0.16">{gem_icon(CARD_W / 2 + 14, 118, 92, color)}</g>',
        # header band (rounded top corners only): points left, bonus gem right
        f'<path d="M1.5,{band} V12.5 A11,11 0 0 1 12.5,1.5 H{CARD_W - 12.5} '
        f'A11,11 0 0 1 {CARD_W - 1.5},12.5 V{band} Z" fill="#FFFFFF" opacity="0.74"/>',
        f'<line x1="1.5" y1="{band}" x2="{CARD_W - 1.5}" y2="{band}" stroke="{frame}" stroke-width="1" opacity="0.35"/>',
        gem_icon(CARD_W - 30, 29, 40, color) if play else gem_icon(CARD_W - 28, 26, 32, color),
    ]
    if int(card.points):
        # a light tint of the bonus colour; white digits outlined in the
        # ribbon's edge colour
        edge = GEM[color][1]
        parts.append(points_label(int(card.points), RIBBON_TINT[color], edge, "#FFFFFF", edge, play))
    # cost tokens in the bottom-left column, largest amount at the top
    # (ties keep the white, blue, green, red, black order)
    costs = sorted(
        ((COLOR_NAMES[i], int(v)) for i, v in enumerate(card.cost) if int(v)),
        key=lambda item: -item[1],
    )
    pitch = 36 if play else 30
    for row, (gem, amount) in enumerate(costs):
        parts.append(cost_token(23 if play else 20, CARD_H - (23 if play else 22) - (len(costs) - 1 - row) * pitch,
                                gem, amount, play))
    if play:
        return "".join(parts)
    # level pips and the engine card id (bottom-right)
    for pip in range(int(card.level)):
        x = CARD_W - 18 - pip * 11
        parts.append(
            f'<polygon points="{x},{CARD_H - 24} {x + 4},{CARD_H - 19} {x},{CARD_H - 14} {x - 4},{CARD_H - 19}" '
            f'fill="{frame}"/>'
        )
    # implementation ids: engine card id and the V3 policy indices of buying it
    first, last = v3_purchase_range(int(card.id))
    parts.append(
        f'<text x="{CARD_W - 11}" y="{CARD_H - 45}" text-anchor="end" font-size="12" font-weight="700" '
        f'fill="#2A2620" {FONT}>ID {int(card.id)}</text>'
        f'<text x="{CARD_W - 11}" y="{CARD_H - 31}" text-anchor="end" font-size="9.5" '
        f'fill="#4A463E" {FONT}>V3 {first}–{last}</text>'
    )
    return "".join(parts)


def noble_body(noble, uid: str, play: bool = False) -> str:
    """Current-edition layout: a points label in the top-left corner, the
    portrait in the middle and the required bonuses in a row along the bottom."""
    nid = int(noble.id)
    band = 94 if play else 102
    parts = [
        f'<rect x="1.5" y="1.5" width="{NOBLE - 3}" height="{NOBLE - 3}" rx="8" fill="#F1E3C2" '
        f'stroke="#8C6A3A" stroke-width="3"/>',
        # bottom band holding the requirements
        f'<path d="M1.5,{band} H{NOBLE - 1.5} V{NOBLE - 9.5} A8,8 0 0 1 {NOBLE - 9.5},{NOBLE - 1.5} '
        f'H9.5 A8,8 0 0 1 1.5,{NOBLE - 9.5} Z" fill="#E2CC97"/>',
        f'<line x1="3" y1="{band}" x2="{NOBLE - 3}" y2="{band}" stroke="#8C6A3A" stroke-width="1" opacity="0.5"/>',
    ]
    robe = ROBES[nid % len(ROBES)]
    hair = HAIR[nid % len(HAIR)]
    parts.append(portrait(70, -14, NOBLE_PORTRAIT[nid], robe, hair))
    parts.append(points_label(int(noble.points), "#E2CC97", "#8C6A3A", "#3E2C14", play=play))
    if not play:
        # implementation ids in the top-right corner
        parts.append(
            f'<text x="{NOBLE - 9}" y="17" text-anchor="end" font-size="11" font-weight="700" '
            f'fill="#4A3820" {FONT}>ID {nid}</text>'
            f'<text x="{NOBLE - 9}" y="29" text-anchor="end" font-size="9" fill="#6A5634" '
            f'{FONT}>V3 {v3_noble_index(nid)}</text>'
        )
    requirements = [(COLOR_NAMES[i], int(v)) for i, v in enumerate(noble.requirement) if int(v)]
    side, gap = (34, 6) if play else (24, 6)
    pitch = side + gap
    start = NOBLE / 2 - (len(requirements) * pitch - gap) / 2
    for column, (gem, amount) in enumerate(requirements):
        parts.append(requirement_tile(start + column * pitch, band + (4 if play else 6), gem, amount, play))
    return "".join(parts)


def card_title(card, play: bool = False) -> str:
    color = COLOR_LABELS_JA[int(card.bonus)]
    cost = "・".join(
        f"{COLOR_LABELS_JA[i]}{int(v)}" for i, v in enumerate(card.cost) if int(v)
    )
    if play:
        return f"レベル{int(card.level)} {int(card.points)}点 ボーナス{color} コスト{cost}"
    first, last = v3_purchase_range(int(card.id))
    return (f"カード ID {int(card.id)} レベル{int(card.level)} {int(card.points)}点 "
            f"ボーナス{color} コスト{cost} V3購入インデックス{first}〜{last}")


def noble_title(noble, play: bool = False) -> str:
    need = "・".join(
        f"{COLOR_LABELS_JA[i]}{int(v)}" for i, v in enumerate(noble.requirement) if int(v)
    )
    if play:
        return f"貴族 {int(noble.points)}点 必要ボーナス{need}"
    return (f"貴族 ID {int(noble.id)} {int(noble.points)}点 必要ボーナス{need} "
            f"V3インデックス{v3_noble_index(int(noble.id))}")


def deck_body(level: int) -> str:
    """Face-down deck: the level's frame colour, a lattice and one gem per level."""
    frame = LEVEL_FRAME[level]
    lattice = "".join(
        f'<line x1="{x}" y1="3" x2="{x - CARD_H}" y2="{CARD_H - 3}"/>'
        f'<line x1="{x - CARD_H}" y1="3" x2="{x}" y2="{CARD_H - 3}"/>'
        for x in range(CARD_H, CARD_W + CARD_H, 28)
    )
    pips = "".join(
        f'<polygon points="{x},{CARD_H / 2 - 9} {x + 9},{CARD_H / 2} {x},{CARD_H / 2 + 9} {x - 9},{CARD_H / 2}" '
        f'fill="{frame}" stroke="#FFFFFF" stroke-width="2"/>'
        for x in (CARD_W / 2 + (i - (level - 1) / 2) * 26 for i in range(level))
    )
    return (
        f'<defs><clipPath id="deck{level}"><rect x="1.5" y="1.5" width="{CARD_W - 3}" '
        f'height="{CARD_H - 3}" rx="11"/></clipPath></defs>'
        f'<rect x="1.5" y="1.5" width="{CARD_W - 3}" height="{CARD_H - 3}" rx="11" fill="#F6F1E4"/>'
        f'<g clip-path="url(#deck{level})" stroke="{frame}" stroke-width="1.5" opacity="0.35">{lattice}</g>'
        f'<rect x="1.5" y="1.5" width="{CARD_W - 3}" height="{CARD_H - 3}" rx="11" fill="none" '
        f'stroke="{frame}" stroke-width="3"/>'
        f'<rect x="12" y="12" width="{CARD_W - 24}" height="{CARD_H - 24}" rx="6" fill="none" '
        f'stroke="{frame}" stroke-width="1.5"/>'
        f'<rect x="{CARD_W / 2 - 46}" y="{CARD_H / 2 - 22}" width="92" height="44" rx="22" '
        f'fill="#F6F1E4" stroke="{frame}" stroke-width="2"/>{pips}'
    )


def svg_document(width: int, height: int, title: str, body: str) -> str:
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">'
        f"<title>{escape(title)}</title>{body}</svg>\n"
    )


def sheet(rows: Sequence[Sequence], item_w: int, item_h: int,
          draw: Callable[[object, str], str], title: str, prefix: str) -> str:
    """Lay out items row by row (each inner sequence is one row)."""
    columns = max(len(row) for row in rows)
    width = columns * item_w + (columns - 1) * GAP
    height = len(rows) * item_h + (len(rows) - 1) * GAP
    body = []
    for r, row in enumerate(rows):
        for c, item in enumerate(row):
            x = c * (item_w + GAP)
            y = r * (item_h + GAP)
            body.append(f'<g transform="translate({x},{y})">{draw(item, f"{prefix}{r}_{c}")}</g>')
    return svg_document(width, height, title, "".join(body))


def check_v3_layout(cards: Sequence) -> None:
    """The purchase ranges must tile the V3 purchase block exactly."""
    expected = int(E3.OFFSET_PURCHASE)
    for card in sorted(cards, key=lambda c: int(c.id)):
        first, last = v3_purchase_range(int(card.id))
        game = cs.Game(0)
        action = cs.Action()
        action.type = cs.ActionType.PURCHASE
        action.card_id = int(card.id)
        if first != expected or int(E3.encode(action, game)) != first:
            raise RuntimeError(f"unexpected V3 purchase layout at card {int(card.id)}")
        expected = last + 1
    if expected != int(E3.OFFSET_PURCHASE) + int(E3.TOTAL_PURCHASE):
        raise RuntimeError("V3 purchase ranges do not cover the purchase block")


def render() -> Dict[Path, str]:
    cards = cs.get_all_cards()
    nobles = cs.get_all_nobles()
    check_v3_layout(cards)
    out: Dict[Path, str] = {}
    for card in cards:
        out[ASSETS / "cards" / f"card_{int(card.id):02d}.svg"] = svg_document(
            CARD_W, CARD_H, card_title(card), card_body(card, f"c{int(card.id)}"))
    play = ASSETS / "play"
    for card in cards:
        out[play / "cards" / f"card_{int(card.id):02d}.svg"] = svg_document(
            CARD_W, CARD_H, card_title(card, True), card_body(card, f"c{int(card.id)}", True))
    for noble in nobles:
        out[play / "nobles" / f"noble_{int(noble.id):02d}.svg"] = svg_document(
            NOBLE, NOBLE, noble_title(noble, True), noble_body(noble, f"n{int(noble.id)}", True))
    for level in (1, 2, 3):
        out[play / "decks" / f"deck_l{level}.svg"] = svg_document(
            CARD_W, CARD_H, f"レベル{level}の山札", deck_body(level))
    for level in (1, 2, 3):
        # one row per bonus colour (white, blue, green, red, black), cheapest
        # prestige first; every level has the same number of cards per colour
        rows = [
            sorted((c for c in cards if int(c.level) == level and int(c.bonus) == color),
                   key=lambda c: (int(c.points), int(c.id)))
            for color in range(len(COLOR_NAMES))
        ]
        if len({len(row) for row in rows}) != 1:
            raise RuntimeError(f"level {level} colours have different card counts")
        out[ASSETS / "cards" / f"level{level}.svg"] = sheet(
            rows, CARD_W, CARD_H, card_body,
            f"レベル{level}の発展カード{sum(len(r) for r in rows)}枚（1行1色）", f"l{level}_")
    for noble in nobles:
        out[ASSETS / "nobles" / f"noble_{int(noble.id):02d}.svg"] = svg_document(
            NOBLE, NOBLE, noble_title(noble), noble_body(noble, f"n{int(noble.id)}"))
    noble_list = list(nobles)
    out[ASSETS / "nobles" / "nobles.svg"] = sheet(
        [noble_list[:6], noble_list[6:]], NOBLE, NOBLE, noble_body,
        f"貴族タイル{len(noble_list)}枚", "n_")
    return out


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="verify that assets/ matches the engine data")
    args = parser.parse_args(argv)
    rendered = render()
    stale: List[Tuple[Path, str]] = []
    for path, text in rendered.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current != text:
            stale.append((path, "missing" if current is None else "outdated"))
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
    if args.check and stale:
        for path, reason in stale:
            print(f"{reason}: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1
    print(f"{len(rendered)} assets {'checked' if args.check else 'written'}"
          f" ({len(stale)} {'stale' if args.check else 'updated'})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
