"""Pure proof/refutation tree selection for DFPN nodes."""

from __future__ import annotations

from typing import Any, Callable, Dict, Iterable, List, Optional


def _first_matching(children: Iterable[Any], *, proof_zero: bool) -> Optional[Any]:
    for child in children:
        if proof_zero and child.proof == 0:
            return child
        if not proof_zero and child.disproof == 0:
            return child
    return None


def selected_children(node: Any, *, want_proof: bool) -> List[Any]:
    """Return exactly the children represented by a proof/refutation tree."""
    if node.node_type == "OR":
        if want_proof:
            selected = _first_matching(node.children, proof_zero=True)
            return [] if selected is None else [selected]
        return [child for child in node.children if child.disproof == 0]
    if want_proof:
        return [child for child in node.children if child.proof == 0]
    selected = _first_matching(node.children, proof_zero=False)
    return [] if selected is None else [selected]


def extract_tree(
    node: Any,
    *,
    want_proof: bool,
    summarize: Callable[[Any], Dict[str, Any]],
) -> Dict[str, Any]:
    data = summarize(node)
    if node.children:
        data["children"] = [
            extract_tree(
                child,
                want_proof=want_proof,
                summarize=summarize,
            )
            for child in selected_children(node, want_proof=want_proof)
        ]
    return data


__all__ = ["extract_tree", "selected_children"]
