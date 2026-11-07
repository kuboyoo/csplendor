# ソルバー実行方法

## 詰みソルバーの CLI

`scripts/dfpn_mate_solver.py` は、任意局面から攻め側（`--attacker`）の強制勝利を、全ての守り側の応手・場の補充の
めくれ・山札予約の全カードについて検証して探します（めくれ検証、`solve_reveal_verified_mate`）。探索本体は C++ の
`solve_reveal_verified_mate_cpp` です（[詰み探索ガイド](mate_usage.md)）。公開カードだけで最短の候補手順を探し、その
prefix を固定して厳密に検証し、証明できなければ固定を緩めます。探索上限に達した場合は `Mate` / `NoMate` と誤確定
せず `Unknown` を返します。盤面入力は `scripts/mate_solver.py` と同じ USI 準拠の指定を使います。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 0 \
  --node-limit 700000 \
  --time-limit 10 \
  --pretty
```

SPN を直接渡す場合も `--position` に `position ...` 形式で指定でき、`position ... moves ...` や `--moves`
（カンマ区切り、または複数回指定）で USI 着手を適用してから探索します。

主なオプションは次の通りです。

- `--attacker 0|1`: 詰みを証明する側のプレイヤー。
- `--node-limit N` / `--time-limit SEC`: 探索ノード・時間の上限（0 は上限なし）。
- `--visible-only-winner`: めくれを無視した公開カードだけのゲームを解く（詰みの証明ではない）。
- `--reveal-proof-dag`: 証明 DAG を出力する（`--proof-dag-format compact|v1|both`、`--proof-dag-node-limit`、`--proof-dag-edge-limit`）。
- `--kifu-output mate.kifu`: 検証済みの主手順を Splendor KIFU（めくれは `reveal:C<id>` 注釈）で保存する。
- `--simple-payment`: 購入の支払いを金温存の 1 通りに限る（完全な検証では指定しない）。
- `--allow-deck-reserve`、`--no-memo`: 公開カードだけの候補探索の設定。
- `--no-proof`: 主手順・証明を出力しない。
- `--pretty`: JSON を整形して出力する。

予約（場札・山札）で11枚になった場合のトークン返却（`RETURN_GEM`）は予約手の一部として扱い、
詰み深さを消費しません。ネイティブのreveal-verified・visible-only solver、
`scripts/mate_solver.py`、`scripts/dfpn_mate_solver.py` のいずれも、返却の選択肢を
貴族選択と同じく着手したプレイヤーの選択として展開します。証明・payloadの辞書は
`waiting_return` を含みます。`--moves` や `position ... moves` では旧来の
`reserve:L2/return:W`・`reserve:C10/return:U` も2手（例: `reserve:L2`、`return:W`）に展開して受け付けます。

終了コードは、`Mate` / `NoMate` の確定時が `0`、入力エラーが `1`、上限到達などで `Unknown` の場合が `2` です。
出力はJSONです。`status` には `Mate`、`NoMate`、`Unknown`、`InvalidInput` のいずれかが入ります。

証明数・反証数で展開先を選ぶ探索（df-pn）は C++ の `solve_reveal_verified_mate_cpp(algorithm="dfpn")` と
`search_reveal_verified_mate_depths(algorithm="dfpn")` で選べます（[設計](mate_dfpn_cpp_design.md)）。Python 版の
df-pn（`DFPNMateSolver`、`--max-depth` で動く旧来の既定モード、`--kifu-dfpn`）は 2026-11-07 に削除しました。

## 証明応手の遅延展開

完全な証明DAGを一括生成せず、表示中のノードだけを展開する場合は
`csplendor.expand_mate_frontier()` を使います。返却される
`csplendor_mate_frontier_v1` は `complete: true` のときだけ利用可能で、攻撃側は
選択された証明手、守備側は全合法手、それぞれの具体的なめくれ結果を含みます。

子ノードの `child_position` は表示用の完全SPN、`child_state` は次回探索用の版付き
スナップショットです。各エントリは `waiting_return` / `child_waiting_return` も含みます。
SPNだけでは `waiting_noble`、`final_round`、`winner` を保持できないため
（返却待ちは手番側の所持数が10枚を超えることから推定されます）、継続探索には必ず
`child_state` を使用してください。探索量は
`max_nodes`、`time_limit_seconds`、1層の出力数は `edge_limit` で制限できます。

## 山札込み詰みの反復深化

深さ `N` の不詰みを確定後に `N + 1` へ進み、最初の詰み、探索上限、または
指定最大深さで安全に停止するには `--reveal-depth-range MIN MAX` を使います。
特定の初手だけを調べる場合は `--required-root-action` にUSI着手を指定します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 1 \
  --reveal-depth-range 5 8 \
  --required-root-action take:WUG \
  --node-limit 10000000 \
  --time-limit 120 \
  --pretty
```

`--node-limit` と `--time-limit` は深さ範囲全体の累積予算です。ある深さが
`Unknown` になった場合、それより深い詰みを見つけても最短性を保証できないため、
既定動作はそこで停止します。`N` 手以内の不詰みから `N + 1` 手以降の不詰みは
導けません。恒久的な不詰みには、深さ付き探索ではなく全到達状態の勝利領域を
解く別の固定点証明が一般には必要です。ただし、終局済みの非勝利局面と、残る
全カード・貴族の点を攻撃側が独占しても15点未満となる局面については、安価で
完全な証明が可能なため `permanent_no_mate` として探索前に停止します。返却値の
`permanent_no_mate_certificate` に根拠と得点上限を含めます。

## CPU並列と実戦AIモード

`--jobs N` は、山札込み探索のルート着手、具体的めくれ、必要に
応じて次の応手層をCPU並列化します。正証明モードでは従来のblank/oracle探索も
1ワーカーで併走し、最初に詰みを証明した探索を採用して残りを協調停止します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 1 \
  --reveal-depth-range 3 8 \
  --reveal-anytime \
  --jobs 16 \
  --time-limit 2
```

`--reveal-anytime` はN手で証明できなかった場合もN+1へ進みます。この場合、返せる
のは正の詰み証明だけであり、`no_mate_found` は不詰みを意味しません。最短性が必要
な解析・問題検査では同オプションを外してください。完全モードは不詰みを確定する
ため攻撃側の全合法手と具体的めくれを列挙するので、大幅に高コストです。

組み込みAIでは `csplendor.MateSearchSession` を対局ごとに保持します。
`search_anytime()` は検証済みの `winning_root_action` を期限内に返し、`cancel()` は
持ち時間管理スレッドから呼べます。`search()` は最短深さを保証する完全モードです。
セッションのexactトランスポジション表は深さ間・手番間で保持され、前回探索した
実子局面に進んだ場合はキャッシュを再利用します。支払いモードが変わった場合と
`clear()` を呼んだ場合は、安全のため表を破棄します。`search_anytime()` でも短い
厳密探索を先行させて表を更新し、浅い深さで得た証明手・反例手は次の深さの手順
序へ使います。表は `max_cache_states`（既定200万局面）で上限を設け、直近の探索で
使ったエントリを優先して保持します。
