# Python API Reference

The `csplendor` package provides a high-level Python interface to the C++ core engine.

## `csplendor.Game`
The main class for controlling game state.

### Constructor
- `Game(seed: int = 0)`: Initializes a new game state with an optional random seed.

### Properties
- `board`: Returns the `Board` object.
- `legal_actions`: Returns a list of all currently legal `Action` objects.
- `requires_forced_pass`: True only when the sole legal action is `PASS`.
- `base_actions`: Returns a filtered list of "base" actions (ignoring return/noble combinations).
- `scores`: Returns a tuple of scores `(player0_score, player1_score)`.
- `turn`: The current turn count.
- `current_player`: The index of the current player (0 or 1).
- `winner`: The winner's index, or -1 if the game is ongoing, -2 for a draw.

### Methods
- `apply(action: Action) -> bool`: Applies an action to the current state.
- `apply_forced_pass(record_history: bool = True) -> bool`: Applies the forced
  pass when no ordinary action exists. If the opponent also cannot act, the
  game ends as a draw.
- `undo() -> bool`: Reverts the last action.
- `is_legal(action: Action) -> bool`: Checks if an action is legal.
- `is_game_over() -> bool`: Returns True if the game has ended.
- `serialize_snapshot() -> bytes`: Serializes the current board, hidden state,
  deck order, and phase without undo history.
- `Game.deserialize_snapshot(snapshot: bytes) -> Game`: Restores a versioned
  lightweight snapshot after validating its rules fingerprint and checksum.
- `Game.snapshot_format_version() -> int`: Returns the binary layout version.
- `Game.snapshot_rules_version() -> int`: Returns the rule-transition version.

Snapshots contain authoritative hidden information. An imperfect-information
search must determinize the restored game for its root observer before use.
See [Versioned Game Snapshot](game_snapshot.md).

### Public card probability API

- `Board.observable_card_pool(observer, level) -> list[int]`: Returns the
  observer-safe union of the physical deck and the opponent's hidden
  reservations for one tier.
- `StateEncoder.encode_public_card_statistics(game, player, observer) ->
  list[float]`: Calculates native posterior summaries including the next-one
  and next-three reveal reachability probabilities.
- `StateEncoder.public_card_feature_size() -> int`: Returns the fixed feature
  length.

The observer argument is mandatory; no full-information physical-deck list is
exposed by this API. The observable pool is invariant under
observer-perspective determinization, and the hidden reservation identity is
never exposed separately.

---

## `csplendor.Action`
Represents a game move.

### Attributes
- `type`: `csplendor.ActionType` (for example `TAKE_DIFFERENT` or `PASS`).
- `take`: List[6] of gems to take (index 0-5).
- `card_id`: ID of the card being purchased or reserved.
- `deck_level`: Level of the deck being reserved (0-2).
- `from_reserved`: Boolean, True if purchasing from hand.
- `gold_as`: List[5] of colors that Gold gems are acting as.
- `return_gems`: List[6] of gems to return if over the limit of 10.
- `noble_choice`: ID of the noble chosen (if multiple eligible).

---

## Static Data Access
- `csplendor.get_card(id: int) -> Card`: Returns the static data for a card.
- `csplendor.get_noble(id: int) -> Noble`: Returns the static data for a noble.
- `csplendor.expand_mate_frontier(game, *, attacker, depth, ...) -> dict`: Verifies and returns only the immediate proof responses for lazy mate replay.
- `csplendor.load_mate_frontier_game(position=..., state=...) -> Game`: Restores a root SPN or an exact lazy-proof child state. Prefer `state` for child nodes because it retains final-round and noble-choice phases.
- `csplendor.search_reveal_verified_mate_depths(game, *, attacker, min_depth, max_depth, ...) -> dict`: Safely tests consecutive reveal-verified depths. A conclusive bounded refutation advances to `N + 1`; mate, `Unknown`, cumulative budget exhaustion, or `max_depth` stops the sweep. It never extrapolates bounded no-mate, but can return `permanent_no_mate` with a terminal or score-ceiling certificate.
- `csplendor.search_reveal_verified_mate_anytime(game, *, attacker, min_depth, max_depth, jobs=..., ...) -> dict`: Deadline-oriented positive-proof search. It may advance after an inconclusive depth and therefore never reports bounded no-mate or minimality.
- `csplendor.MateSearchSession(attacker, *, jobs=..., max_cache_states=2_000_000)`: Reusable AI-facing search session with cooperative cancellation and a bounded exact transposition table retained across depths and turns. It reuses exact descendant results and shallower-depth move ordering. Use `search_anytime()` for live play, `search()` for minimal-depth analysis, and `clear()` between games.
- `csplendor.MateSearchCancellationToken`: Cooperative cancellation token accepted by the stateless mate-search APIs.
