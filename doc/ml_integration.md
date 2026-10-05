# 機械学習・AI連携

[README](../README.md) / [ドキュメント索引](index.md)

## 状態特徴量V1（196要素）

`StateFeaturizer.featurize(game, observer=...)` は `float32` のNumPy配列を返します。定義は [encoding_schema.h](../src/encoding_schema.h) と [state_encoder.h](../src/state_encoder.h) が正本です。

| offset | 要素数 | 内容 | スケーリング |
|---:|---:|---|---|
| 0 | 6 | 銀行のトークン | 全色とも `/7` |
| 6 | 36 | player 0 | トークン6 `/10`、ボーナス5 `/10`、点数1 `/15`、予約3×8 |
| 42 | 36 | player 1 | 同上 |
| 78 | 96 | 公開カード12×8 | レベル順・スロット順 |
| 174 | 3 | 山札枚数 | 各レベル `/40` |
| 177 | 18 | 貴族3×6 | 点数 `/3`、要求5色 `/4` |
| 195 | 1 | 手番 | player ID `0` または `1` |

カード1枚の8要素は `[点数/5, コスト白/7, 青/7, 緑/7, 赤/7, 黒/7, bonus ID/5, level/3]` です。カードID自体は含みません。空きスロットはゼロ、相手の伏せ予約はレベルだけを残します。正規化はclippingではないため、点数などが1を超えることがあります。

```python
import csplendor as cs

game = cs.Game(seed=42)
observer = game.current_player
features = cs.StateFeaturizer().featurize(game, observer=observer)
assert features.shape == (196,)
canonical = cs.StateEncoder.encode_canonical(game, player=observer, observer=observer)
assert len(canonical) == 196
```

`observer=-1`（既定）は完全情報です。対戦AIでは `0` / `1` を明示します。`encode_canonical()` の `player` は特徴量内のプレイヤー順、`observer` は情報公開範囲の指定であり、別の意味です。

V1に `waiting_noble`・`waiting_return`・`final_round` の独立した特徴はありません（`V3SearchSession` は `pending_decision_features` で `waiting_return`・`waiting_noble` の2次元を追加します。[V3探索](mcts_v3.md)）。新モデルに追加する場合は、既存196要素の意味を変えず別schemaとして定義してください。`StateEncoder.schema_version()`、`schema_fingerprint()`、`schema_sections()`、`gem_color_ids()` で既存契約を取得できます。

公開カード統計は `StateEncoder.encode_public_card_statistics(game, player, observer)` で別途取得できます。長さは `public_card_feature_size()` を参照し、モデル独自の入力と混同しないでください。

## 行動空間

| エンコーダ | サイズ | 意味 |
|---|---:|---|
| Python `ActionEncoder` / native `ActionEncoderCpp` | 48 | 基本行動。返却・支払いを圧縮。返却フェーズはslot 0..5を返却色に再利用 |
| `ActionEncoderV2` | 4869 | 場・予約スロットを基準に全選択肢を表現 |
| `ActionEncoderV3` | 3133 | 購入をカードID、貴族を貴族IDで表現。返却フェーズは表現不可 |
| `ActionEncoderV4` | 3121 | V3の山札予約返却を分離し、`RETURN_GEM` 6枠を追加 |

全行動policyにはV4を利用します。山札予約で11枚になると同じ手番のまま返却フェーズ（`board.waiting_return`）に入り、合法手は `RETURN_GEM` だけになります。V2/V3はこれを表現できず、返却フェーズのマスクは空、`encode` は `-1` です。詳細は [V2仕様](action_space_v2.md) / [V3仕様](action_space_v3.md) / [V4仕様](action_space_v4.md)。V3で学習したpolicyの移行（`v3_to_v4_table()`）もV4仕様にあります。`Game.legal_actions` の添字や `Action.pack()` の整数と、policyの行動IDは別物です。

```python
import numpy as np
import csplendor as cs

game = cs.Game(seed=42)
game.simple_payment_mode = False
mask = cs.ActionEncoderV4.get_action_mask(game)
action_id = int(np.flatnonzero(mask)[0])
action = cs.ActionEncoderV4.decode(action_id, game)
assert cs.ActionEncoderV4.encode(action, game) == action_id
assert game.apply(action)
```

policy の教師データや推論で合法手の ID 列が必要な場合は、`cs.ActionEncoderV4.legal_action_ids(game)`（V3 ID は `cs.ActionEncoderV3.legal_action_ids(game)`）を使えます（`game.legal_actions` と同じ順の int32 配列）。Python で1手ずつ `encode` するより約20倍速く、結果は同じです。

着手する局面のマスクでpolicyを制限します。終局時はV2/V3/V4のマスクが全ゼロです。モデルには行動schemaのversion・fingerprintと支払いモードも保存してください。

## 内蔵MCTSとの接続

内蔵C++ MCTSは48枠の `ActionEncoderCpp` を使用します。V4の3121枠policyには `V3SearchSession`（[V3探索](mcts_v3.md)。IDはV4）または明示的な変換が必要です。48枠では返却・支払いの全パターンを独立した枝として選べません。

48枠の区分は異色取得 `0..9`、同色取得 `10..14`、公開予約 `15..26`、山札予約 `27..29`、場から購入 `30..41`、予約から購入 `42..44`、貴族選択 `45..47` です。返却フェーズ中だけ、slot `0..5` を白・青・緑・赤・黒・金の返却に再利用します。

パス枠はありません。rootで `game.requires_forced_pass` なら先に `game.apply_forced_pass()` で進めます。探索内部の強制パスはnative側で処理します。

非公開情報を扱う場合は観測者視点のdeterminizationを使い、実際の山札順や相手の伏せ予約を探索入力へ漏らさないでください。[並列MCTSの例](parallel_mcts_usage.md)、[情報集合の契約](information_state.md) も参照してください。
