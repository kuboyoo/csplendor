# Engine Specifications

This document describes the internal logic and data structures of the `csplendor` engine.

## 1. Data Structures

### GemType (Color)
Gems are represented as integers (0-5):
- 0: EMERALD (Green)
- 1: SAPPHIRE (Blue)
- 2: RUBY (Red)
- 3: DIAMOND (White)
- 4: ONYX (Black)
- 5: GOLD (Yellow / Wildcard)

### Card
- `id`: Unique identifier (0-89).
- `level`: Tier 1, 2, or 3.
- `points`: Prestige points (0-5).
- `bonus`: The `GemType` this card provides upon purchase.
- `cost`: Array of 5 integers representing the costs in Green, Blue, Red, White, and Black.

### Noble
- `id`: Unique identifier (0-9).
- `points`: Prestige points (always 3).
- `requirement`: Array of 5 integers representing the bonus gems needed to attract this noble.

## 2. Game State

### PlayerState
Each player's state includes:
- `gems`: Current gem counts (index 0-5).
- `bonuses`: Bonus gem counts from purchased cards (index 0-4).
- `points`: Total prestige points.
- `reserved`: List of IDs of cards currently reserved (max 3).
- `purchased_cards`: List of IDs of cards currently owned.

### Board
The global state includes:
- `bank`: Available gems in the central bank (index 0-5).
- `visible`: 3x4 grid of card IDs currently on the board.
- `decks`: 3 decks of card IDs (unexplored).
- `nobles`: List of noble IDs available to visit.
- `current_player`: 0 or 1.
- `turn`: Turn count (increments after both players move).

## 3. Game Flow

1. **Initialization**: Board is populated with cards, 3 nobles are selected (for 2-player), and bank is set to 4 gems per color (5 gold).
2. **Turn Sequence**:
    - Player 0 takes an action.
    - End-of-turn check (Nobles visit).
    - Player 1 takes an action.
    - End-of-turn check (Nobles visit).
    - Score check (if someone >= 15 points, enter final round).
3. **Winner Determination**:
    - Highest prestige points.
    - Tie-break: Fewest cards purchased (purchased_count).
    - Hard tie: Draw.

## 4. Legal Move Generation
The engine generates all possible legal moves in a given state:
1. **Take 3 Different**: Take 1 gem each of 3 different colors (excluding Gold).
2. **Take 2 Same**: Take 2 gems of the same color (if >= 4 available in bank).
3. **Reserve Visible**: Take a card from the board and 1 Gold gem (if available).
4. **Reserve Deck**: Take a card from a deck (blind) and 1 Gold gem (if available).
5. **Purchase**: Buy a card from the board or reserved hand. The engine generates **all valid payment combinations** using normal gems and Gold tokens. Each combination is a separate action with a unique `gold_as` array specifying how Gold tokens substitute for each color.
