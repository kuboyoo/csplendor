# ソルバー実行方法

## DFPN詰みソルバー

改良版のDFPN詰みソルバーは `scripts/dfpn_mate_solver.py` で実行します。盤面入力は `scripts/mate_solver.py` と同じUSI準拠の指定を使います。

```bash
python scripts/dfpn_mate_solver.py --position "position startpos 2" --attacker 0 --max-depth 2 --pretty
```

SPNを直接渡す場合も `position` コマンドとして指定できます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position bank:W4U4G4R4K4D5 | visible:L1[0,1,2,3]L2[40,41,42,43]L3[70,71,72,73] | decks:36,26,16 | nobles:[0,1,2] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | 0" \
  --attacker 0 \
  --max-depth 2 \
  --pretty
```

USIの `moves` 付き入力も受け付けます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position startpos 2 moves take:WUG reserve:C12" \
  --attacker 0 \
  --max-depth 3
```

局面をファイルから読む場合は `--position-file` を使います。ファイルには `position ...` 形式、またはSPN本文を記述します。

```bash
python scripts/dfpn_mate_solver.py --position-file position.txt --attacker 0 --max-depth 3 --pretty
```

追加のUSI着手をコマンドラインで適用する場合は `--moves` を使います。カンマ区切り、または複数回指定できます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position startpos 2" \
  --moves "take:WUG,reserve:C12" \
  --moves "buy:C12/pay:W0U0G0R0K0D0" \
  --attacker 0 \
  --max-depth 3
```

主なオプションは次の通りです。

- `--attacker 0|1`: 詰みを証明する側のプレイヤー。
- `--max-depth N`: 攻撃側手番を何手先まで読むか。
- `--node-limit N`: 探索ノード上限。
- `--time-limit SEC`: 探索時間上限。
- `--allow-deck-reserve`: 山札予約手も合法手候補に含める。
- `--no-memo`: 置換表を使わない。
- `--no-proof`: 証明木・反証木を出力しない。
- `--pretty`: JSONを整形して出力する。

終了コードは、`Mate` / `NoMate` の確定時が `0`、入力エラーが `1`、上限到達などで `Unknown` の場合が `2` です。

出力はJSONです。`status` には `Mate`、`NoMate`、`Unknown`、`InvalidInput` のいずれかが入ります。`stats.root_proof_number` と `stats.root_disproof_number` でルート局面の証明数・反証数を確認できます。

## 5手以上の検証

山札のめくれを含む5手以上の詰みは、候補探索と全めくれ検証を分ける
`--reveal-verified` を推奨します。候補手順を得た後、全防御手、場の補充、
山札予約の全カードを検証し、探索上限に達した場合は `Mate` / `NoMate` と
誤確定せず `Unknown` を返します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 0 \
  --reveal-verified \
  --node-limit 700000 \
  --time-limit 10 \
  --pretty
```

通常のPython DFPNでも、枝刈り候補は未展開子として保持されます。ANDノードの
証明とORノードの反証では、未展開の合法手・めくれをすべて具体化してからのみ
確定します。脅威評価、依存関係、返却パターンは探索順序と遅延生成にだけ使われ、
異なる次局面を同一視しません。

## 証明応手の遅延展開

完全な証明DAGを一括生成せず、表示中のノードだけを展開する場合は
`csplendor.expand_mate_frontier()` を使います。返却される
`csplendor_mate_frontier_v1` は `complete: true` のときだけ利用可能で、攻撃側は
選択された証明手、守備側は全合法手、それぞれの具体的なめくれ結果を含みます。

子ノードの `child_position` は表示用の完全SPN、`child_state` は次回探索用の版付き
スナップショットです。SPNだけでは `waiting_noble`、`final_round`、`winner` を保持
できないため、継続探索には必ず `child_state` を使用してください。探索量は
`max_nodes`、`time_limit_seconds`、1層の出力数は `edge_limit` で制限できます。

## 山札込み詰みの反復深化

深さ `N` の不詰みを確定後に `N + 1` へ進み、最初の詰み、探索上限、または
指定最大深さで安全に停止するには `--reveal-depth-range MIN MAX` を使います。
特定の初手だけを調べる場合は `--required-root-action` にUSI着手を指定します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 1 \
  --reveal-verified \
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

`--jobs N` は通常DFPNに加えて、山札込み探索のルート着手、具体的めくれ、必要に
応じて次の応手層をCPU並列化します。正証明モードでは従来のblank/oracle探索も
1ワーカーで併走し、最初に詰みを証明した探索を採用して残りを協調停止します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 1 \
  --reveal-verified \
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
