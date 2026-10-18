# ActionEncoderV5 行動空間仕様

> Schema metadata: `ActionEncoderV5.schema_version()`、
> `schema_fingerprint()`、`schema_sections()`。sizeとsection offsetのC++正本は
> `encoding_schema.h`の`ActionSpaceV5`である。

> **Version**: V5 (3049 actions)
> **Header**: `src/action_encoder_v5.h`
> **Python**: `csplendor.ActionEncoderV5`
> **Fingerprint**: `csplendor.action.v5;size=3049;layout=840,140,12,3,2035,12,6,1`

## 概要

V5は全行動policy用の行動空間である。予約（場札・山札とも）は返却を持たず、予約で所持トークンが
10枚を超えた場合は、場札の補充・山札から引いたカードを見てから、別の手番内行動 `RETURN_GEM` で
1枚返す（[返却フェーズ](#返却フェーズ)）。取得の返却は従来どおり同じActionに含む。

| Category | Offset | Size | 内容 |
|---|---:|---:|---|
| TAKE_DIFFERENT | 0 | 840 | 10 combos × 84 return patterns（[V3](action_space_v3.md)と同じ） |
| TAKE_SAME | 840 | 140 | 5 colors × 28 return patterns（V3と同じ） |
| RESERVE_VISIBLE | 980 | 12 | `980 + level * 4 + slot`。返却なし |
| RESERVE_DECK | 992 | 3 | `992 + level`。返却なし |
| PURCHASE | 995 | 2035 | card ID × 支払パターン（V3 id − 90） |
| VISIT_NOBLE | 3030 | 12 | noble ID（V3 id − 90） |
| RETURN_GEM | 3042 | 6 | `3042 + color`（W,U,G,R,K,D = 0..5） |
| PASS | 3048 | 1 | - |
| **Total** | | **3049** | |

TAKE_DIFFERENT・TAKE_SAME・PURCHASE・VISIT_NOBLEの内部エンコード（返却パターン、card別支払パターン、
noble ID）はV3と同一である。式はV3仕様を参照し、`OFFSET_PURCHASE` / `OFFSET_VISIT_NOBLE` を
V5の値に読み替える。

## Action ID Calculation（V3から変わる部分）

### RESERVE_VISIBLE (offset 980, size 12)

```
level, slot = 予約する場札の位置（level 0-2、slot 0-3）
action_id = 980 + level * 4 + slot
```

`decode` は `card_id` にその位置の場札を入れ、返却なしの `RESERVE_VISIBLE` を返す。
`return_gems` が非ゼロの予約は現行ルールで合法にならず、`encode` は `-1` を返す。

### RESERVE_DECK (offset 992, size 3)

```
action_id = 992 + level
```

### RETURN_GEM (offset 3042, size 6)

```
color = returned color (0=W, 1=U, 2=G, 3=R, 4=K, 5=D)
action_id = 3042 + color
```

1 actionで返すのは常に1枚である（`return_gems` の1要素だけが1）。C++では `Action::returned_color()` で色を取得でき、`Action::pack()` も色を保持する。

### PASS (offset 3048, size 1)

強制パスの実actionであり、終局局面のマスクは全ゼロである。

## 返却フェーズ

- 予約（場札・山札）で所持トークンが11枚（10枚＋金1枚）になった場合、エンジンは返却フェーズに入る。
  手番は同じplayerのまま `Board.waiting_return == True`（`Board.pending_decision == 1`）となる。
- 返却は、場札の予約なら補充された場札を、山札の予約なら引いたカードを見た後に選ぶ。
- 返却フェーズの合法手は `RETURN_GEM`（`ActionType` 値7）だけで、手番playerが所持する色
  （金を含む6色）ごとに1つ、各1枚を返す。
- 処理順は「返却 → 貴族」である。返却後に通常の手番終了処理（貴族判定、必要なら
  `waiting_noble`、手番交代）を行う。`waiting_return` と `waiting_noble` が同時に立つことはない。
- 返却フェーズ中だけ、手番playerは11枚を保持できる。
- 異色取得・同色取得の返却は従来どおり同じActionに含まれる。購入は返却を持たない。
- 山札から引いたカードは相手には非公開のまま、補充された場札と返却色は公開情報である。

### 他のエンコーダでの扱い

| エンコーダ | 返却フェーズ |
|---|---|
| `ActionEncoderV5` | `RETURN_GEM` 6枠で表現する |
| `ActionEncoderV3` / `ActionEncoderV2` | 表現できない。マスクは空、`encode` は `-1`。予約は返却なしの枠だけを使う |
| `ActionEncoderCpp`（48枠） | 貴族選択と同様に枠を再利用し、slot `0..5` を返却色に割り当てる |

## Python Helpers

- `ActionEncoderV5.ACTION_SIZE`、`OFFSET_TAKE_DIFFERENT`、`OFFSET_TAKE_SAME`、
  `OFFSET_RESERVE_VISIBLE`、`OFFSET_RESERVE_DECK`、`OFFSET_PURCHASE`、`TOTAL_PURCHASE`、
  `OFFSET_VISIT_NOBLE`、`OFFSET_RETURN_GEM`、`OFFSET_PASS`。
- `ActionEncoderV5.get_action_mask(game)`: 3049枠のマスク（終局時は全ゼロ）。
- `ActionEncoderV5.legal_action_ids(game)`: `game.legal_actions` と同じ順のV5 IDの `int32` 配列。
- `ActionEncoderV5.encode` / `decode` / `decode_and_match`: 単一actionの変換。
- `csplendor.v5_semantic_group_id(v5_id)`: 主判断の群番号（139群。異色取得0..9、同色取得10..14、
  場札予約15..26、山札予約27..29、購入 `30 + card id`、貴族 `120 + noble id`、PASS 132、返却133..138）。

## 旧行動空間V4について

V4（3121枠）は場札予約の返却を同じActionに含めていた（返却を補充前に選ぶ）。2026-10-18に
ルールを変えてV5に置き換え、`ActionEncoderV4` は削除した。V4で学習したモデル・データは
そのままでは使えない（移行の手段は提供しない）。V4 idとの関係は、取得は同じID、購入・貴族は
`V4 id − 72`、`RETURN_GEM` は `V4 id − 72`、PASSは `3120 → 3048` である。

モデルには行動schemaのversion（5）・fingerprint・支払いモードを保存する。
