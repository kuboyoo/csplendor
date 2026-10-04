# V3行動空間のネイティブ多対局探索（`V3SearchSession`）

[README](../README.md) / [ドキュメント索引](index.md)

`src/mcts_v3.h` は、`ActionEncoderV3`（3,133 ID。全支払い・全返却を別の枝として保持）
の上で動く PUCT 探索です。既存の内蔵 MCTS（48 枠固定）とは独立した実装で、
複数の対局を 1 つの session に登録して同時に進め、葉の評価要求を 1 つのバッチにまとめます。
学習用の自己対局・大量評価が目的で、探索の枝刈りやルール判断は増やしていません。

## 設計

- **多対局ロックステップ**: `collect()` は登録済みの全対局の木を降り、対局ごとに最大
  `leaf_batch_size` 個の新しい葉を集めて特徴量と合法手 ID を返します。`apply()` で
  事前確率と価値を入れ、逆伝播します。両関数の中で Python オブジェクトに触れないため、
  binding は GIL を解放します。
- **決定化**: シミュレーションごとに root の観測者視点で `Game::shuffled_clone` を作り、
  その世界で木を降ります。木は観測者の情報集合木で、公開行動列で節点に到達します。
- **サンプル型 chance 節点**: 公開補充（場のカード購入・予約後の補充）と観測者自身の
  山札予約では、めくれたカード ID を key とする chance 節点を経由します。各シミュレーションは
  自分の世界でめくれた結果の枝を通るため、chance 節点の値は一様なめくれの経験平均になります。
  相手の山札予約は観測者に見えないので分岐せず、情報集合の中に留まります。
- **列挙型 chance 節点（任意、2026-09-30 追加）**: `chance_enumeration_depth = D`（既定 0）で、
  root から D 手以内の chance 節点は初回到達時にその段の観測者未見カード
  （`Board::observable_card_pool`: 山札＋相手の非公開予約）を 1 枚ずつ次のめくれに固定した
  子局面をすべて作り、1 バッチで評価します（予算は 1 シミュレーション分）。chance 節点は
  子の値の平均（`chance_risk_weight` で行動側にとって最悪の値を混合）から始まります。
  終局する子は評価せず勝敗を使います。以後のシミュレーションは従来どおり世界のめくれに従います。
  `chance_control_variate = true` では、その後の逆伝播で chance 節点とその上の節点に
  v − v0(めくれ) + mean(v0) を渡し、めくれの抽選による分散を除きます（下の部分木には v のまま）。
  `stats()` の `chance_enumerations` / `chance_rows` で回数と評価行数を確認できます。
- **二段階選択（任意）**: `semantic_groups=true` で「支払い・返却を畳んだ主判断」（133 群）で
  PUCT を行い、選んだ群の中でもう一度 PUCT を行います。既定は平坦な PUCT です。
- **virtual loss**: 1 バッチ内の葉が重ならないように、収集中の経路に仮訪問を加えます。
  評価待ちの節点を再び選んだ場合、その対局はそのラウンドの収集を終えます。
- **未展開時に見えなかった合法手**: 世界によって合法手集合が異なる場合、展開時に事前確率を
  持たなかった手には `unseen_action_prior`（既定 1e-3）を与えます。
- **終局・深さ上限**: 終局は勝敗（±1、引き分け 0）、`max_depth` 到達は `draw_value` で逆伝播します。
  強制パスは PASS（3132）を唯一の合法手として扱います。
- **対局単位のマルチスレッド（任意、2026-10-04 追加）**: `num_threads`（既定 1）を 2 以上にすると、
  `collect()` / `apply()` が対局ごとに独立したスレッドで処理されます。各対局は自分の木・乱数・作業領域だけを
  使い、葉は slot 順に連結され、`apply()` も対局ごとに行の順序を保ちます。そのため、バッチの並び・乱数列・
  木の更新はスレッド数によらず同一です。`num_threads=1` は従来の逐次処理と同じ経路です。
  1 プロセスで多数の対局を探索する場合に有効です。複数プロセスで並列実行している場合は、プロセス数と合わせて
  CPU コア数を超えないようにしてください。

含まれないもの: 詰み探索、rollout、手をまたぐ木の再利用、シミュレーション途中の相手手番での
再決定化、反復回避。これらは呼び出し側（dlsplendor）で必要に応じて重ねます。

## 入力特徴量

葉の特徴量は `StateEncoder::encode_canonical(world, player, observer=player)`（196）に、
`public_card_features` で `encode_public_card_statistics`（117）、`physical_seat_feature` で
座席符号（先手 −1／後手 +1）を連結したもので、既定は 314 次元です。

## Python API

```python
import csplendor as cs
config = cs.V3SearchConfig()
config.num_simulations = 800
config.leaf_batch_size = 32
config.num_threads = 4              # 任意。既定 1（結果はスレッド数によらず同一）
session = cs.V3SearchSession(config)
slot = session.add_game(game, observer=game.current_player, seed=1, root_noise=False)
while not session.all_done():
    features, legal_ids, offsets, slots = session.collect()   # features: (N, 314)
    if len(slots) == 0:
        break
    priors, values = evaluate(features, legal_ids, offsets)    # priors は legal_ids と同じ並び
    session.apply(legal_ids, offsets, priors, values)
visits = session.root_visits(slot)          # {action_id: visits}
q = session.root_action_values(slot)        # {action_id: root 視点の Q}
session.reset_game(slot, next_game, observer, seed)
```

`v3_semantic_group_id(action_id)` は主判断の群番号を返します。

## 検証

`tests/test_mcts_v3.py`: 予算どおりの訪問数、root の訪問手が合法手集合に含まれること、
同一 seed の再現性、バッチ形状、価値の符号（深さ 1 のみの構成で厳密検証）、root ノイズ、
二段階選択、不正な観測者の拒否、`num_threads` を変えても同じ探索結果になること。
`tests/v3_session_parallel_unit.cpp`: スレッド数 1/2/4/16 で全バッチと木がビット一致すること
（rollout・chance 列挙を含む構成も対象。ThreadSanitizer でも実行）。

速度の目安（dlsplendor 側の計測、Ryzen 9 7900X＋RTX 4060 Ti、selfplay21 の 3.4M パラメータ網、
800 探索、64 局同時、1 Python thread）: 約 5,400 局/時、約 6.5 万 sim/秒。
Python 実装の木探索は 12 プロセスで約 420 局/時でした。詳細は dlsplendor 側の
`doc/native_v3_search_20260929.md` を参照してください。

上の値は 2026-10-04 の高速化より前の計測です。この高速化では、NN 推論を除いたエンジン部分が
1 スレッドで 1.31〜1.43 倍になりました（結果はビット一致）。さらに `num_threads=16` では、従来の逐次処理の
約 8 倍になります。条件は [速度ベンチマーク](performance_benchmarks.md) を参照してください。
