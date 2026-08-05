"""Token conversion primitives for the canonical USI gem alphabet."""

import re
from typing import Dict, List, Sequence, Tuple

from ..gem_colors import GEM_USI_SYMBOLS

GEM_LETTERS: Tuple[str, ...] = tuple(GEM_USI_SYMBOLS)
LETTER_TO_GEM: Dict[str, int] = {
    letter: index for index, letter in enumerate(GEM_LETTERS)
}


def safe_int(value, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def counts_to_letters(
    counts: Sequence[int], *, include_gold: bool = True
) -> str:
    output: List[str] = []
    end = 6 if include_gold else 5
    for index in range(end):
        count = safe_int(counts[index]) if index < len(counts) else 0
        if count > 0:
            output.append(GEM_LETTERS[index] * count)
    return "".join(output)


def letters_to_counts(token: str, *, allow_gold: bool = True) -> List[int]:
    counts = [0, 0, 0, 0, 0, 0]
    for letter in token.strip().upper():
        if letter not in LETTER_TO_GEM:
            raise ValueError(f"invalid gem letter: {letter}")
        index = LETTER_TO_GEM[letter]
        if index == 5 and not allow_gold:
            raise ValueError("gold is not allowed in this token")
        counts[index] += 1
    return counts


def gold_as_to_token(gold_as: Sequence[int]) -> str:
    parts: List[str] = []
    for index in range(5):
        count = safe_int(gold_as[index]) if index < len(gold_as) else 0
        if count > 0:
            parts.append(f"{GEM_LETTERS[index]}{count}")
    return "".join(parts)


def token_to_gold_as(token: str) -> List[int]:
    token = token.strip().upper()
    output = [0, 0, 0, 0, 0]
    for letter, number in re.findall(r"([WUGRK])(\d+)", token):
        output[LETTER_TO_GEM[letter]] += int(number)
    canonical = "".join(
        f"{GEM_LETTERS[index]}{output[index]}"
        for index in range(5)
        if output[index] > 0
    )
    if canonical != token:
        raise ValueError(f"invalid gold assignment token: {token}")
    return output


def pay_counts_to_token(pay: Sequence[int]) -> str:
    values = [int(pay[index]) if index < len(pay) else 0 for index in range(6)]
    return (
        f"W{values[0]}U{values[1]}G{values[2]}"
        f"R{values[3]}K{values[4]}D{values[5]}"
    )


def token_to_pay_counts(token: str) -> List[int]:
    token = token.strip().upper()
    if not token:
        raise ValueError("empty pay token")
    output = [0, 0, 0, 0, 0, 0]
    index = 0
    while index < len(token):
        letter = token[index]
        if letter not in LETTER_TO_GEM:
            raise ValueError(f"invalid pay token letter: {letter}")
        index += 1
        number_end = index
        while number_end < len(token) and token[number_end].isdigit():
            number_end += 1
        if number_end == index:
            raise ValueError(f"missing pay token count after {letter}")
        output[LETTER_TO_GEM[letter]] += int(token[index:number_end])
        index = number_end
    return output
