# Web API Reference

The Splendor engine can be accessed over HTTP using a FastAPI-based web server. This is ideal for browser-based GUI development.

## 1. Running the Server
```bash
# From the project root
uvicorn csplendor.api:app --reload
```
The server will be available at `http://localhost:8000`. OpenAPI (Swagger) documentation can be viewed at `http://localhost:8000/docs`.

## 2. API Endpoints

### `POST /game`
Creates a new game session.
- **Query Params**: `seed` (int, optional).
- **Response**: `{"session_id": "uuid-string"}`.

### `GET /game/{session_id}`
Returns the current full state of the game.
- **Response**: `GameStateSchema` (JSON).

### `POST /game/{session_id}/action`
Applies a legal action to the game.
- **Query Params**: `action_idx` (int) - The index of the action within the `legal_actions` list returned by the state.
- **Response**: `GameStateSchema` (Updated state).

### `POST /game/{session_id}/undo`
Undoes the last action.
- **Response**: `GameStateSchema` (Updated state).

## 3. JSON Schema Overview (Simplified)

### `GameStateSchema`
```json
{
  "board": {
    "bank": [4, 4, 4, 4, 4, 5],
    "visible_cards": [[...], [...], [...]],
    "deck_counts": [40, 30, 20],
    "nobles": [1, 5, 9],
    "current_player": 0,
    "turn": 0,
    "game_over": false,
    "winner": -1
  },
  "players": [
    {
      "index": 0,
      "gems": [0, 0, 0, 0, 0, 0],
      "bonuses": [0, 0, 0, 0, 0],
      "points": 0,
      "reserved_cards": [],
      "purchased_cards": []
    },
    ...
  ],
  "legal_actions": [
    {
      "type": 0,
      "take": [1, 1, 1, 0, 0, 0],
      "card_id": null,
      ...
    }
  ]
}
```

## 4. Integration for GUI
The GUI should:
1. Call `POST /game` to start a session.
2. Call `GET /game/{session_id}` to get the initial layout.
3. Map user interactions (clicks) to the indices in the `legal_actions` array.
4. Call `POST /game/{session_id}/action?action_idx=X` to progress the game.

## 5. PURCHASE Actions and Payment Options

The engine generates **all valid payment combinations** for each purchasable card.

### `gold_as` Field
Each PURCHASE action includes a `gold_as` array (5 elements, one per color: Green, Blue, Red, White, Black).
- `gold_as[i]` indicates how many Gold tokens are used as color `i`.
- Example: `gold_as: [0, 2, 1, 0, 0]` means 2 Gold used as Blue, 1 Gold used as Red.

### Multiple Actions per Card
The same `card_id` may appear in multiple PURCHASE actions with different `gold_as` values.
```json
{"type": 4, "card_id": 15, "gold_as": [0, 0, 0, 0, 0]},
{"type": 4, "card_id": 15, "gold_as": [1, 0, 0, 0, 0]},
{"type": 4, "card_id": 15, "gold_as": [0, 1, 0, 0, 0]}
```

### GUI Implementation Tips
1. **Simple UI**: Pick any valid action for the card (e.g., the first one).
2. **Detailed UI**: Filter actions by `card_id`, then let the user choose how to pay.
