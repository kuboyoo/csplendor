# Gumbel AlphaZero の root 探索（`V3SearchConfig.gumbel`、2026-10-13）

[README](../README.md) / [ドキュメント索引](index.md) / [ネイティブ多対局探索](mcts_v3.md)

`src/mcts_v3.h` の PUCT 探索に、Gumbel AlphaZero（Danihelka ら 2022）の root 選択と非 root 選択を加える。
目的は学習側（dlsplendor）の方策改善の質を上げること: 少ない探索でも「探索後の方策が網の方策より良い」ことが
保証される（Policy improvement）ので、同じ GPU 時間で質の良い教師を多く作れる。既定は無効（`gumbel = false`）で、
従来の PUCT と出力はビット単位で同じ。

## 設定

| 項目 | 既定 | 意味 |
|---|---|---|
| `gumbel` | `false` | root を Gumbel top-k + 逐次半減、非 root を改善方策による決定的選択にする |
| `gumbel_m` | 16 | root の候補数の上限（合法手がそれより少なければ全部） |
| `gumbel_c_visit` | 50 | σ(q) = (c_visit + max_a N(a)) · c_scale · q の c_visit |
| `gumbel_c_scale` | 1.0 | 同 c_scale |

`gumbel = true` のとき、root の Dirichlet ノイズ（`add_game(..., root_noise=True)`）は使わない（探索の多様性は Gumbel
ノイズが担う）。`semantic_groups`、`set_root_visit_floor`、`rollout_samples > 0` とは併用しない（`V3SearchConfig` の検査で
例外）。決定化・chance 節点・virtual loss・多スレッド・`advance_game` は従来どおり。

## root の選択（逐次半減）

1. root の事前確率が入った時点で、各合法手 a に Gumbel ノイズ g(a) = −log(−log u)（u は対局の乱数列）を 1 回だけ引く。
   logit(a) = log(prior(a))。
2. 候補集合 = logit + g の上位 m' = min(m, 合法手数)。
3. 予算 n = `num_simulations` − 1（root 自身の評価が 1 回分）を、⌈log2 m'⌉ 段の半減で使う。段 p で残り候補が k 個のとき、
   各候補は累計で ⌊n / (⌈log2 m'⌉ · k)⌋（最低 1）回まで訪問される（論文の配分）。段の中では、割当（訪問 + 仮訪問）が
   その段の上限に達していない候補のうち割当の少ないものから選ぶ。候補が全部上限に達し、評価待ちが残っていれば、
   その対局はこのラウンドの収集を終える（衝突と同じ扱い）。全部完了したら、候補を
   score(a) = g(a) + logit(a) + σ(q̂(a)) の上位 ⌈k/2⌉ に減らす。q̂(a) = root から見た子の Q（未訪問なら root の Q）。
4. 最後の段が終わるか予算を使い切ったら終了。**選ぶ手** `root_gumbel_action()` は残った候補の score 最大。
   予算を使い切っても候補が 2 個以上残っていれば、その時点の score で決める。

木の再利用（`advance_game`）で root に既存の訪問があるときは、上限を既存の訪問込みで数える（既存の訪問が多い候補は
その段では追加されない）。候補と g は新しい root で引き直す。

## 非 root の選択

展開済みの決定節点では、この世界の合法手 a について
π'(a) ∝ prior(a) · exp(σ(q̂(a)))、σ = (c_visit + max_a N(a)) · c_scale、q̂(a) = 子の Q（親から見た符号）、未訪問は親の Q
を使い（completed Q）、argmax_a [π'(a) − N(a) / (1 + Σ_b N(b))] を選ぶ。N は訪問 + 仮訪問（1 バッチ内で葉が重ならないため）。
未展開時に見えなかった手の prior は `unseen_action_prior`。chance 節点は従来どおり世界のめくれに従う。

## 出力

- `root_improved_policy(slot)` → `{action_id: 確率}`: root の全合法手について π'(a) ∝ clean_prior(a) · exp(σ(q̂(a)))。
  学習の方策教師はこれを使う（訪問数分布ではない）。
- `root_gumbel_action(slot)` → 選ぶ手の ID。
- `root_visits` / `root_action_values` / `root_value` は従来どおり（archive には訪問数と Q も残す）。

## dlsplendor 側（`dlsplendor/search/native_v3.py`）

`NativeV3Settings.gumbel` / `gumbel_m` / `gumbel_c_visit` / `gumbel_c_scale` を `V3SearchConfig` に渡す。`gumbel` のとき、
`finish_root` は `root_gumbel_action` を返し、`policy_visit_counts` に `root_improved_policy` を入れる（方策の枝刈りはしない）。
自己対局の温度サンプリングは使わず（Gumbel ノイズが探索を担う）、root ノイズのフラグは無視される。
詰み探索・準詰みの上書きは従来どおり。

## 検証

- `tests/test_mcts_v3.py`: 予算・合法性・決定性（同じ seed で同じ結果）、root の訪問が候補 m' 個以内に集中すること、
  `root_gumbel_action` が候補の中にあり最終候補で score 最大であること、`root_improved_policy` が全合法手で和 1 であること、
  価値が偏る評価器で高 Q の手の π' が prior より大きくなること、`gumbel_c_scale = 0` で π' = prior、
  `semantic_groups` / visit floor / rollout との併用が例外になること。
- 棋力: dlsplendor のプール実測（800 探索、`gumbel=true` と `false` を同じ網で it182・R1 と 400 局ずつ）。
