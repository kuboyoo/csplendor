# Python API Reference

The `csplendor` package provides a high-level Python interface to the C++ core engine.

## `csplendor.Game`
The main class for controlling game state.

### Constructor
- `Game(seed: int = 0)`: Initializes a new game state with an optional random seed.

### Properties
- `board`: Returns the `Board` object.
- `legal_actions`: Returns a list of all currently legal `Action` objects.
- `base_actions`: Returns a filtered list of "base" actions (ignoring return/noble combinations).
- `scores`: Returns a tuple of scores `(player0_score, player1_score)`.
- `turn`: The current turn count.
- `current_player`: The index of the current player (0 or 1).
- `winner`: The winner's index, or -1 if the game is ongoing, -2 for a draw.

### Methods
- `apply(action: Action) -> bool`: Applies an action to the current state.
- `undo() -> bool`: Reverts the last action.
- `is_legal(action: Action) -> bool`: Checks if an action is legal.
- `is_game_over() -> bool`: Returns True if the game has ended.

---

## `csplendor.Action`
Represents a game move.

### Attributes
- `type`: `csplendor.ActionType` (e.g., `TAKE_DIFFERENT`).
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
