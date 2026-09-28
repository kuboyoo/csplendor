# 詰み探索・証明・問題集生成

[README](../README.md)の詰み探索APIの補足です。以下のコマンドはリポジトリルートで実行します。
外部AIによる問題集生成には別途 `dlsplendor` とモデルが必要です。


`scripts/dfpn_mate_solver.py` は、任意局面から player0 または player1 の強制勝利を探索します。

実用上は、公開カードだけで候補手順を高速探索し、その後に未公開カードのめくれ、相手の全応手、全支払いパターン、局面入力後の山札予約結果を検証する `--reveal-verified` モードを推奨します。めくれ検証では visible-only の最短主手順 prefix を固定して先に厳密検証し、証明できなかった場合は固定範囲を緩め、最後に通常の幅広い検証へ戻ります。

```bash
python scripts/dfpn_mate_solver.py \
  --position 'bank:... | visible:... | decks:... | nobles:... | P0:... | P1:... | 0' \
  --attacker 0 \
  --reveal-verified \
  --time-limit 30 \
  --pretty
```

完全な詰み応手を確認する場合は、証明に関係する局面だけを DAG 形式で出力できます。同一局面はノード ID で共有されるため、木を単純展開するよりメモリ使用量を抑えられます。

```bash
python scripts/dfpn_mate_solver.py \
  --position '...' \
  --attacker 0 \
  --reveal-verified \
  --reveal-proof-dag \
  --proof-dag-format compact \
  --proof-dag-node-limit 100000 \
  --proof-dag-edge-limit 500000 \
  --time-limit 30
```

証明 DAG は `proof_tree.verification.proof_dag` に返ります。攻撃側は証明に採用した手、守備側は全合法応手、山札予約は全ドロー結果を保持します。既定の `compact` 形式では、同じ action/child に進む複数の具体めくれカードをカードID bitset の reveal group としてまとめ、edge は action template と reveal group への参照で保存します。具体カード集合は保持するため、全めくれに対する応手情報は失いません。従来の辞書型 DAG が必要な場合は `--proof-dag-format v1`、比較用に両方出す場合は `--proof-dag-format both` を指定します。complete DAG は返却前に全 edge を合法手として再走査し、検査済みなら `validated: true` になります。上限超過時も詰み判定結果は維持し、DAG のみ破棄して理由を返します。

完全DAGが大きすぎる場合は、`csplendor.expand_mate_frontier()` で現在局面の1層だけを検証・展開できます。攻撃側ノードでは証明手1手の全めくれ、守備側ノードでは全合法手の全めくれを返します。各 edge の `child_state` は、SPNに含まれない終局・最終ラウンド・貴族選択待ちも保持する版付きスナップショットです。次の呼び出しでは `load_mate_frontier_game(state=...)` で復元します。

```python
import csplendor as cs

game = cs.load_mate_frontier_game(position=position)
frontier = cs.expand_mate_frontier(game, attacker=1, depth=5)
edge = frontier["edges"][0]
child = cs.load_mate_frontier_game(state=edge["child_state"])
next_frontier = cs.expand_mate_frontier(
    child,
    attacker=1,
    depth=edge["child_depth"],
)
```

深さを1手ずつ増やして最初の詰みを調べる場合は
`cs.search_reveal_verified_mate_depths()` を使います。不詰みを確定した深さだけを
通過し、詰み、`Unknown`、累積予算、または最大深さで停止します。
終局済みの非勝利局面、または残る全カード・貴族の点を攻撃側が独占しても15点に
届かない局面では、深さに依存しない `permanent_no_mate` 証明で直ちに停止します。

```python
depth_search = cs.search_reveal_verified_mate_depths(
    game,
    attacker=1,
    min_depth=5,
    max_depth=8,
    max_nodes=10_000_000,
    time_limit_seconds=120,
)
```

実戦AIでは、同一対局中に1個の `MateSearchSession` を保持します。`search_anytime()`
は浅い深さが未確定でも次へ進み、正の詰み証明だけを返すため、持ち時間内の着手
選択に向いています。anytime探索中も一部のCPU予算で厳密置換表を育て、次の深さ
では浅い証明手・反例手を合法手順序へ反映します。相手応手後の局面が前手番の表に
あれば、同じ深さの結果は1ノードの参照で再利用されます。

```python
session = cs.MateSearchSession(
    attacker=ai_player,
    jobs=16,
    max_cache_states=2_000_000,
)

result = session.search_anytime(
    game,
    min_depth=1,
    max_depth=8,
    time_limit_seconds=2.0,
)
if result["status"] == "mate":
    action = cs.Action.unpack(result["winning_root_action"])
    # 通常のAI候補より優先して、この検証済み着手を選択する

# 対局終了・別対局開始時だけ破棄する
session.clear()
```

最短手数まで保証する解析用途では `session.search()` または
`search_reveal_verified_mate_depths()` を使います。こちらは各N手不詰みを全合法手・
具体的めくれについて確定してからN+1へ進むため、実戦向けより高コストです。
思考時間を外部から打ち切る場合は `session.cancel()` を別スレッドから呼べます。

確認用の主手順を `splendorgui` で再生する場合は、`--kifu-output mate.kifu` を追加します。`--kifu-output` は既定で `--reveal-verified` を有効化し、検証済み候補主手順を Splendor KIFU として保存します。通常の DFPN 証明木から主手順を保存する場合は `--kifu-dfpn` も指定します。具体的なめくれカードを持つ DFPN 証明木では、棋譜コメントに `reveal:C<id>` 注釈を出力します。

`--simple-payment` を指定すると、購入時の支払いをゴールド温存パターンに限定できます。完全検証が必要な場合は指定しないでください。

### 詰め問題集の生成

`scripts/generate_mate_puzzles.py` は、`dlsplendor.search.genbu_adapter.GenbuAdapter` を使った Genbu AI 同士の対局から終盤局面を生成し、めくれまで検証済みの詰みだけを問題集として保存します。ランダムに選んだ終盤開始手数に到達した後は、詰みが初めて見つかるまで1手番ごとに候補局面を検証します。AI 対局中だけ簡易支払いモードを有効にします。詰み検証では通常支払いモードに戻し、購入時の全支払いパターン、局面入力後の山札予約、めくれを検証します。

```bash
python scripts/generate_mate_puzzles.py \
  --output-dir generated/mate_puzzles \
  --count 100 \
  --max-attempts 10000 \
  --genbu-weights scripts/weights/genbu.pt \
  --genbu-simulations 100 \
  --min-depth 5 \
  --max-depth 7 \
  --no-strategy-dag \
  --mate-jobs 16 \
  --uniqueness-jobs 16 \
  --time-limit 30
```

旧Genbuモデルの実行に必要な `alphazero-general-ori` は、`dlsplendor` 直下、
同階層、および標準workspaceの `workspace/src/alphazero-general-ori` から自動検出します。
別の場所に置く場合は `ALPHAZERO_ORI_PATH=/path/to/alphazero-general-ori` を指定してください。
Numbaキャッシュは既定で一時ディレクトリへ保存するため、旧ソースツリーが読み取り専用でも
実行できます。

進捗は attempt 開始と詰み探索開始時に表示されます。`--min-losing-alternatives` を1以上にした場合は誤答側詰み探索も表示されます。棄却時は `stage=rejected`、棄却理由、完全な SPN `position` を表示します。Genbu 対局中の定期表示間隔は `--progress-seconds` で変更できます。

高コストなめくれ検証の前に、点差、合法手数、両者の楽観的な近未来得点、visible-only 探索で候補を絞ります。既定では両者が3手以内に15点へ到達しうる合法手12個以上の局面を対象とし、depth 3以上の詰みだけを採用します。詰み証明後は全合法初手を固定して再検証し、別解がない問題だけを保存します。条件は `--threat-turns`、`--min-legal-actions`、`--min-optimistic-score`、`--min-depth`、`--visible-prefilter-time-limit`、`--uniqueness-time-limit` で調整できます。`--uniqueness-max-depth 8` のように指定すると、各初手を問題の詰み深さから指定深さまで反復深化し、より長い別解も除外します。各初手では最初の `--uniqueness-positive-time-limit` 秒（既定2秒）で別解の正証明を高速に探し、残りの累積予算で全合法手・具体的めくれの不詰みを厳密検証します。各初手の累積予算は `--uniqueness-node-limit` と `--uniqueness-time-limit`、候補局面・誤答側の詰み探索は `--mate-jobs 16`、初手間のCPU並列数は `--uniqueness-jobs 16`（いずれも `0` は論理CPU数）で指定します。誤答時に相手の詰みまで成立することは既定の採用条件ではありません。必要なら `--min-losing-alternatives 1` 以上を指定します。

生成物は `depth_XX/<問題ID>/` に分類されます。`XX` はソルバー上の攻撃側手数深さです。各問題には局面情報 `problem.json`、検証済み代表手順 `answer.kifu`、代表手順とDAG状態を収める `strategy.json` が含まれます。DAG作成を有効にした場合は、攻撃側の証明手、守備側の全合法応手、公開カード補充と山札予約を含む全めくれ結果を保持し、同一局面をノードIDで共有します。既定ではDAG作成を試みますが、ノード・辺上限を超えても詰み判定と代表手順が検証済みなら保存します。完全DAGを必須にする場合は `--require-complete-dag`、DAG作成を省いて生成を優先する場合は `--no-strategy-dag` を指定します。DAGの有無と省略理由は `problem.json` の `strategy_dag` および `quality` に保存されます。`strategy.json` は既定で compact DAG を保存し、めくれカード集合は reveal group の bitset として厳密に残します。従来形式が必要な場合は生成時に `--strategy-dag-format v1`、比較用に両方残す場合は `--strategy-dag-format both` を指定します。めくれ候補は現在の山札だけから取り、同じレベル・点数・ボーナス・コストのカードは同型として代表だけを検証します。公開カード補充は一度 blank として進めた局面から反例になりやすい reveal を推定し、危険度の高い候補から検証します。非公開カードを即購入・即予約する oracle 手は合法手順DAGには出力されません。再現性を保つため、生成物内の SPN は伏せ予約カードを `?C<id>` 形式で保存します。通常の公開用 SPN における `?L<level>` と異なり、伏せ予約であることと実カードIDの両方を保持します。購入済みカードは `bought:[<id>,...]`、取得済み貴族は player section の `nobles:[<id>,...]` に保存します。
