# ActionEncoderV4 行動空間仕様

> Schema metadata: `ActionEncoderV4.schema_version()`、
> `schema_fingerprint()`、`schema_sections()`。sizeとsection offsetのC++正本は
> `encoding_schema.h`の`ActionSpaceV4`である。

> **Version**: V4 (3121 actions)
> **Header**: `src/action_encoder_v4.h`
> **Python**: `csplendor.ActionEncoderV4`
> **Fingerprint**: `csplendor.action.v4;size=3121;layout=840,140,84,3,2035,12,6,1`

## 概要

V4は[V3](action_space_v3.md)から山札予約の返却を切り離した行動空間である。
現行ルールでは山札予約（`RESERVE_DECK`）は返却を持たず、予約後に所持トークンが
10枚を超えた場合は、めくれたカードを見てから別の手番内行動 `RETURN_GEM` で1枚返す
（[返却フェーズ](#返却フェーズ)）。

V3との差は次の2点だけである。

- `RESERVE_DECK` の 3 levels × 7 返却パターン（21枠）を 3 levels（3枠）に畳む。
- 6枠の `RETURN_GEM` section（白・青・緑・赤・黒・金）を追加する。

| Category | Offset | Size | 内容 |
|---|---:|---:|---|
| TAKE_DIFFERENT | 0 | 840 | V3と同じ（10 combos × 84 return patterns） |
| TAKE_SAME | 840 | 140 | V3と同じ（5 colors × 28 return patterns） |
| RESERVE_VISIBLE | 980 | 84 | V3と同じ（12 slots × 7 return patterns） |
| RESERVE_DECK | 1064 | 3 | level 0..2。返却なし |
| PURCHASE | 1067 | 2035 | V3 id − 18（card ID × 支払パターン） |
| VISIT_NOBLE | 3102 | 12 | V3 id − 18（noble ID） |
| RETURN_GEM | 3114 | 6 | `3114 + color`（W,U,G,R,K,D = 0..5） |
| PASS | 3120 | 1 | - |
| **Total** | | **3121** | |

### V3との比較

| Metric | V3 | V4 | Change |
|---|---:|---:|---|
| RESERVE_DECK actions | 21 | 3 | −18（返却を分離） |
| RETURN_GEM actions | 0 | 6 | +6 |
| Total actions | 3133 | 3121 | −12 |

TAKE_DIFFERENT・TAKE_SAME・RESERVE_VISIBLE・PURCHASE・VISIT_NOBLEの内部エンコード
（返却パターン、card別支払パターン、noble ID）はV3と同一である。式はV3仕様を参照し、
`OFFSET_PURCHASE` / `OFFSET_VISIT_NOBLE` / `OFFSET_PASS` をV4の値に読み替える。
公開予約・取得の返却は従来どおり同じActionに含まれる（atomic inline return）。

## Action ID Calculation（V3から変わる部分）

### RESERVE_DECK (offset 1064, size 3)

```
level = deck level (0-2)
action_id = 1064 + level
```

`return_gems` が非ゼロの山札予約は現行ルールで合法にならず、`encode` は `-1` を返す。

### RETURN_GEM (offset 3114, size 6)

```
color = returned color (0=W, 1=U, 2=G, 3=R, 4=K, 5=D)
action_id = 3114 + color
```

1 actionで返すのは常に1枚である（`return_gems` の1要素だけが1）。C++では `Action::returned_color()` で色を取得でき、`Action::pack()` も色を保持する。

### PASS (offset 3120, size 1)

```
action_id = 3120
```

V3と同じく強制パスの実actionであり、終局局面のマスクは全ゼロである。

## 返却フェーズ

- 山札予約で所持トークンが11枚（10枚＋金1枚）になった場合、エンジンは返却フェーズに入る。
  手番は同じplayerのまま `Board.waiting_return == True`（`Board.pending_decision == 1`）となる。
- 返却フェーズの合法手は `RETURN_GEM`（`ActionType` 値7）だけで、手番playerが所持する色
  （金を含む6色）ごとに1つ、各1枚を返す。返却はめくれたカードを見た後に選ぶ。
- 処理順は「返却 → 貴族」である。返却後に通常の手番終了処理（貴族判定、必要なら
  `waiting_noble`、手番交代）を行う。`waiting_return` と `waiting_noble` が同時に立つことはない。
- 返却フェーズ中だけ、手番playerは11枚を保持できる。
- 異色取得・同色取得・公開予約の返却は従来どおり同じActionに含まれる。購入は変更なし。
- めくれたカードは相手には非公開のまま、返却色は公開情報である。

### 他のエンコーダでの扱い

| エンコーダ | 返却フェーズ |
|---|---|
| `ActionEncoderV4` | `RETURN_GEM` 6枠で表現する |
| `ActionEncoderV3` / `ActionEncoderV2` | 表現できない。マスクは空、`encode` は `-1` |
| `ActionEncoderCpp`（48枠） | 貴族選択と同様に枠を再利用し、slot `0..5` を返却色に割り当てる |

## Python Helpers

- `ActionEncoderV4.ACTION_SIZE`、`OFFSET_TAKE_DIFFERENT`、`OFFSET_TAKE_SAME`、
  `OFFSET_RESERVE_VISIBLE`、`OFFSET_RESERVE_DECK`、`OFFSET_PURCHASE`、`TOTAL_PURCHASE`、
  `OFFSET_VISIT_NOBLE`、`OFFSET_RETURN_GEM`、`OFFSET_PASS`。
- `ActionEncoderV4.get_action_mask(game)`: 3121枠のマスク（終局時は全ゼロ）。
- `ActionEncoderV4.legal_action_ids(game)`: `game.legal_actions` と同じ順のV4 IDの `int32` 配列。
- `ActionEncoderV4.encode` / `decode` / `decode_and_match`: 単一actionの変換。
- `ActionEncoderV4.v3_to_v4_id(v3_id)`: V3 id → V4 id（V3範囲外は `-1`）。
- `ActionEncoderV4.v4_to_v3_id(v4_id)`: V4 id → V3 id。`RETURN_GEM` は `-1`、山札予約は
  返却なしのV3山札予約ID（`1064 + level * 7`）に戻る。
- `ActionEncoderV4.v3_to_v4_table()`: 長さ3133の `int32` 配列 `t`（`t[v3_id] = v4_id`）。

### V3 → V4 の対応

V3の全IDが決定的にV4へ写る。

| V3 id | V4 id |
|---|---|
| `0..1063` | 同じID |
| `1064..1084`（山札予約21枠。返却なし3枠＋返却付き18枠） | `1064 + (v3_id - 1064) // 7`（同じlevelの山札予約） |
| `1085..3131`（購入・貴族） | `v3_id - 18` |
| `3132`（PASS） | `3120` |

`RETURN_GEM`（`3114..3119`）へ写るV3 IDはない。

## Migration Notes

### From V3 to V4（ML利用者向け）

1. **Policy headのサイズ**: 3133 → 3121。V3で記録したpolicy targetは
   `v3_to_v4_table()` で合算して移行する。

   ```python
   import numpy as np
   import csplendor as cs

   table = cs.ActionEncoderV4.v3_to_v4_table()        # int32[3133]
   v4 = np.zeros(cs.ActionEncoderV4.ACTION_SIZE, np.float32)
   np.add.at(v4, table, v3_policy)                    # 山札予約の7枠は同じlevelへ合算
   ```

   V3の学習済み出力層を流用する場合も、同じ表で重みを並べ替え・合算する。
   `RETURN_GEM` の6枠は対応するV3出力がないため新規に初期化する。
2. **返却フェーズの学習データ**: V3データには返却フェーズの局面がない。V4での自己対局により
   `RETURN_GEM` の局面を新たに得る必要がある。
3. **状態特徴量**: `V3SearchSession` は `V3SearchConfig.return_phase_feature`（既定 `True`）で
   返却フェーズを示す1次元（`waiting_return` のとき1.0）を座席特徴の後に追加する。
   既定設定では `state_dim` が従来より1増える。従来の次元を維持する場合は `False` にする。
   詳細は [V3多対局探索](mcts_v3.md)。
4. **Semantic group**: `cs.v4_semantic_group_id(v4_id)` は139群（0..132はV3と同じ配置、
   PASSは132、`RETURN_GEM` は133..138）。`v3_semantic_group_id` はV3 ID用に従来の意味を保つ。
5. **局面の保存**: Game snapshotはformat/rules version 2になった。version 1のsnapshotは
   `Game.deserialize_snapshot` で拒否されるため、`Game.upgrade_snapshot_v1(bytes)` で一度だけ
   変換する（実行時の後方互換はない）。[snapshot](game_snapshot.md) を参照。
6. **情報集合ID**: information stateはformat 3 / rules 2になり、旧bytesとは一致しない。
   定石DBのキーは再生成する。[情報集合](information_state.md) を参照。
7. モデルには行動schemaのversion（4）・fingerprint・支払いモードを保存する。

### Model Compatibility

V4はV3の出力層と次元が異なる。上の表でV3 policyを移行できるが、山札予約の返却を
めくれたカードを見て選ぶ方針はV3データからは得られない。
