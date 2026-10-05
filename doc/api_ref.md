# Python API Reference

The `csplendor` package provides a high-level Python interface to the C++ core engine.

## `csplendor.Game`
The main class for controlling game state.

### Constructor
- `Game(seed: int = 0)`: Initializes a new game state with an optional random seed.

### Properties
- `board`: Returns the `Board` object.
- `simple_payment_mode`: `False` (Game default) enumerates all valid payments;
  `True` selects the minimum-gold payment per purchase. The HTTP creation API
  defaults to `True`, so set the mode explicitly when integrating clients.
- `blank_refill_mode`: Analysis-only blank refill; leave `False` for normal play.
- `legal_actions`: Returns a list of all currently legal `Action` objects.
- `legal_action_codes`: Compact `Action.pack()` values in legal-action order.
- `legal_action_count`: Number of legal actions without constructing Python Actions.
- `requires_forced_pass`: True only when the sole legal action is `PASS`.
- `base_actions`: Returns a filtered list of "base" actions (ignoring return/noble combinations).
- `scores`: Returns a tuple of scores `(player0_score, player1_score)`.
- `turn`: The current turn count.
- `current_player`: The index of the current player (0 or 1).
- `winner`: The winner's index, or -1 if the game is ongoing, -2 for a draw.

### Methods
- `apply(action: Action) -> bool`: Applies an action to the current state.
- `apply_action_code(code: int, record_history: bool = True) -> bool`: Validates
  and applies a packed action. Policy IDs and legal-list indices are not codes.
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
  Only the current format/rules version (2) is accepted; a version 1 snapshot
  is rejected with "csplendor game snapshot version 1 must be converted with
  Game.upgrade_snapshot_v1 first".
- `Game.upgrade_snapshot_v1(snapshot: bytes) -> bytes`: One-off migration of a
  version 1 snapshot to the current version (version 1 positions never have a
  pending token return). There is no runtime backward compatibility.
- `Game.snapshot_format_version() -> int`: Returns the binary layout version.
- `Game.snapshot_rules_version() -> int`: Returns the rule-transition version.
- `serialize_information_state(observer: int) -> bytes`: Returns a versioned,
  observer-safe canonical identity for persistent analysis storage. It omits
  deck order and the opponent's hidden reservation IDs and cannot be restored
  as an authoritative `Game`.
- `information_state_hash(observer: int) -> int`: Returns a stable 64-bit
  index for the serialized information state. Persistent stores must compare
  the bytes as well rather than treating the hash as collision-free identity.
- `Game.information_state_format_version() -> int`: Returns the information
  identity layout version.
- `Game.information_state_rules_version() -> int`: Returns its rule-semantic
  version.

Snapshots contain authoritative hidden information. An imperfect-information
search must determinize the restored game for its root observer before use.
See [Versioned Game Snapshot](game_snapshot.md).
See [Versioned information-state identity](information_state.md) for opening
analysis and book keys.

### Board decision phases
- `Board.waiting_noble`: True while the player to move must choose a noble
  (`VISIT_NOBLE` only). Kept for compatibility with `pending_decision`.
- `Board.waiting_return`: True while the player to move must return one token
  after a deck reservation that left them with 11 tokens (`RETURN_GEM` only;
  the player keeps the turn and may hold 11 tokens until the return).
- `Board.pending_decision` (read-only int): `0` none, `1` return a token
  (`waiting_return`), `2` choose a noble (`waiting_noble`). Both flags are never
  set at once (state invariant `invalid_pending_decision`).

Seed 0 is deterministic, like any other seed. The current implementation uses
the low 32 bits to seed `mt19937`. Store the engine revision alongside seeds.
`Board.players` and `get_player()` return copies; editor changes must be written
back with `board.set_player(index, player)`. Prefer `Game.apply()` for game play.
Trusted apply methods bypass legality checks and must not receive network input.

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
  `RETURN_GEM` (value 7) returns exactly one token during the return phase.
- `take`: List[6] of gems to take (index 0-5).
- `card_id`: ID of the card being purchased or reserved.
- `deck_level`: Level of the deck being reserved (0-2).
- `from_reserved`: Boolean, True if purchasing from hand.
- `gold_as`: List[5] of colors that Gold gems are acting as.
- `return_gems`: List[6] of gems to return if over the limit of 10. Take and
  visible-reservation actions carry their returns inline; a deck reservation
  never does (the return is a following `RETURN_GEM` action). For
  `RETURN_GEM` exactly one entry is 1.
  `pack()` encodes the returned colour and `repr(action)` is
  `"RETURN_GEM: c"` (the C++ `Action::returned_color()` helper is not bound to
  Python; read the colour from `return_gems`).
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

## V4 action encoder helpers

`ActionEncoderV4` (3121 ids) is V3 with the deck-reservation returns split into
a 6-slot `RETURN_GEM` section. See [Action space V4](action_space_v4.md).

- `ActionEncoderV4.get_action_mask(game) -> numpy.ndarray`: 3121-slot legality mask.
- `ActionEncoderV4.encode` / `decode` / `decode_and_match`: Convert single actions.
- `ActionEncoderV4.legal_action_ids(game) -> numpy.ndarray`: V4 ids of
  `game.legal_actions` in order, as an `int32` array.
- `ActionEncoderV4.v3_to_v4_id(v3_id)` / `v4_to_v3_id(v4_id)`: Id conversion
  (`v4_to_v3_id` returns `-1` for `RETURN_GEM`).
- `ActionEncoderV4.v3_to_v4_table() -> numpy.ndarray`: `int32[3133]` map for
  migrating V3 policy targets (`np.add.at(v4, table, v3)`).
- `ActionEncoderV4.schema_version()` / `schema_fingerprint()` / `schema_sections()`.

## V3 action encoder helpers

V3 cannot represent `RETURN_GEM`: during the return phase its mask is empty and
`encode` returns `-1`.

- `ActionEncoderV3.get_action_mask(game) -> numpy.ndarray`: 3133-slot legality mask.
- `ActionEncoderV3.encode(action, game) -> int` / `decode(action_id, game) -> Action` /
  `decode_and_match(action_id, game) -> Action`: Convert single actions.
- `ActionEncoderV3.legal_action_ids(game) -> numpy.ndarray`: The V3 ids of
  `game.legal_actions`, in the same order, as an owning `int32` array. Equal to
  `[ActionEncoderV3.encode(a, game) for a in game.legal_actions]` (including the
  forced `PASS` id 3132 and `-1` for an action the encoder cannot represent), but
  produced by one native enumeration without creating Python `Action` objects.

## Native V3 search (`V3SearchSession`, experimental)

`V3SearchConfig` fields are documented in [V3 multi-game search](mcts_v3.md).
The session works on V4 action ids. `V3SearchConfig.pending_decision_features`
(default `True`) appends two pending-decision features after the seat feature:
`waiting_return` then `waiting_noble` (1.0 when set), so the default `state_dim`
is 316 (314 + 2); set it to `False` to omit them.
`csplendor.v4_semantic_group_id(v4_id)` gives the 139 decision groups used by
the session; `v3_semantic_group_id` keeps its V3 meaning.
`V3SearchConfig.num_threads` (default `1`, must be positive) runs `collect()` and
`apply()` with one worker per game. Rows are concatenated in slot order, so the
collected batches, RNG streams and trees are identical for every thread count.
Both calls release the GIL.

## State feature arrays

- `StateEncoder.encode(game, observer=-1) -> list[float]`: The existing list API is unchanged.
- `StateEncoder.encode_numpy(game, observer=-1) -> numpy.ndarray`: Returns an independent,
  owning, writable, C-contiguous `float32` array of shape `(196,)`.
- `StateFeaturizer.featurize(game, observer=-1) -> numpy.ndarray`: Uses `encode_numpy`
  directly, without an intermediate Python list.

The feature schema, numerical operations, and observer rules are identical to `encode`.
Use observer 0 or 1 to hide the opponent's private reservation identities; -1 requests full information.
Changing a returned array never changes the game or another result. Retained arrays remain valid
after subsequent calls, game mutation/destruction, and MCTS searches. The GIL stays held.
No caller-owned output buffer is accepted, so there is no partial-write or capacity contract to manage.
Existing invalid-argument/card exceptions remain; an unsuccessful call does not modify earlier arrays.
