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

### `POST /game/{session_id}/ai_move`
Optional compatibility bridge to external AI projects. It is not required by
the rule/session/replay API and model code is not bundled in csplendor. Without
torch and the external `dlsplendor` package the endpoint returns HTTP 503.
Unknown AI modes and missing fixed search budgets return HTTP 400 before any
model stack is loaded. The `ml` package extra supplies torch only;
`dlsplendor` and its models must be provided separately by the integrating
project.

### Legacy replay endpoints

`GET /replay/files`, `POST /replay/load?path=<filename>`, and
`GET /replay/{session_id}/game/{game_idx}/{step}` expose the optional legacy
pickle replay viewer. Pickle files are executable input and must therefore be
created or installed only by a trusted server administrator. `/replay/load`
accepts only `.pkl` files that resolve inside the configured replay data
directory; uploads, arbitrary paths resolving outside it, and symlinks escaping
that directory are rejected. The listing endpoint returns directory-local names
and does not unpickle files merely to inspect them. This endpoint is not an
upload or untrusted-data ingestion API; use a non-executable serialization
format if replays must cross a trust boundary.

## 3. JSON Schema Overview (Simplified)

Gem arrays always use `Diamond/White, Sapphire/Blue, Emerald/Green, Ruby/Red, Onyx/Black, Gold` order. Five-element arrays omit Gold.

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

If the only legal action has type `6` (`PASS`), it is a forced transition and
may be submitted like any other legal-action index. If neither player has an
ordinary move, applying it ends the game as a draw.

## 5. PURCHASE Actions and Payment Options

The engine generates **all valid payment combinations** for each purchasable card.

### `gold_as` Field
Each PURCHASE action includes a `gold_as` array (5 elements, one per color: White, Blue, Green, Red, Black).
- `gold_as[i]` indicates how many Gold tokens are used as color `i`.
- Example: `gold_as: [0, 2, 1, 0, 0]` means 2 Gold used as Blue, 1 Gold used as Green.

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
