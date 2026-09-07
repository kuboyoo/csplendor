# 双方の勝ち筋を使う詰み探索（2026-09-07）

本書の計測は最初のルート優先順位版（`24cb05f`）の記録。
続く厳密な枝刈り・反証手再利用・DFPNの検証では、深さ6〜8の全78初手を
解決できた。最新の実測と判定範囲は [厳密な枝刈りとDFPN](mate_exact_pruning_dfpn.md) を参照。

## 対象と判定範囲

`bga_910749228.kifu` の50手終了後、P0が51手目を選ぶ局面。
`tests/fixtures/mate_routes_bga_910749228_ply50.spn` は、その局面だけを
取り出した完全なSPNであり、プレイヤー名などの棋譜ヘッダーは含まない。
P0は8点、P1は10点、P0の合法手は78手。

探索の `depth` は攻撃側の行動回数であり、両者を合わせた手数ではない。
補充は棋譜で実際に出たカードに固定せず、残り山札の全カードを対象とする。

## 通常の詰み探索の変更

- 双方について、資源・割引・予約カード・貴族を使った3購入先までの
  単独購入ルートを見積もり、勝ち筋を早める手と相手を遅らせる手を優先する。
- 補充候補も、次の手番のプレイヤーだけでなく双方の勝ち筋から並べる。
- 同種局面で成功した手・応手を、合法性を確認して優先する。
- 見積もりを上限付きキャッシュに保存する。このキャッシュは証明には使わない。
- 残り1回の行動では15点に届かない枝、P1が即座に勝てる局面、
  最終ラウンドの確定結果を、厳密な点数・購入枚数比較で解決する。
- 証明済みの結果を深さの単調性に従って再利用する。候補限定の失敗を、
  別の深さの厳密な不詰みとして再利用しない。

ルート見積もりは最短手数の厳密計算ではない。見積もり用のビーム幅6・
購入回数3は手の並べ替えにだけ使い、通常の厳密探索の合法手や補充を削らない。
証明用の枝刈りはこの見積もりと分離している。
探索ノード上限は追加していない。

`use_route_ordering=False` で比較用の従来探索に戻せる。
新しい既定値は `True` であり、GUIの既存の詰みAPI呼び出しにも適用される。
UI側にエンジンの判断ロジックは追加していない。

## 反証探索（明示的に指定する実験用モード）

P0の詰みを反証する場合の
`solve_reveal_verified_mate_cpp(..., attacker=1,
cooperative_reveals=True, exact_reveal_search=True, include_proof_dag=False)`
は、P0の全行動に対し、P1の応手と合法な補充を選ぶことでP1勝利に導けるかを調べる。
成功は「P0には全補充に対する確定勝ちがない」という反証であり、
「P1がどの補充でも勝つ」という通常の詰み証明ではない。

`exhaustive_attacker_actions=False` の場合、P1と補充の候補だけを絞る。
P0の行動は絞らないので成功した反証は有効だが、失敗は必ず `unknown` と扱う。
通常の詰みDAGと混同しないよう、このモードのDAG出力は拒否する。
GUIからの通常の詰み判定では、この実験用モードはまだ自動実行しない。

## 再現コマンド

リポジトリ直下で実行する。`max_nodes=0` でノード数は無制限。

```bash
python setup.py build_ext --inplace
PYTHONPATH=. python scripts/benchmark_mate_routes.py tests/fixtures/mate_routes_bga_910749228_ply50.spn --depth 3 --seconds 10 --baseline
PYTHONPATH=. python scripts/benchmark_mate_routes.py tests/fixtures/mate_routes_bga_910749228_ply50.spn --depth 3 --seconds 10
PYTHONPATH=. python scripts/benchmark_mate_routes.py tests/fixtures/mate_routes_bga_910749228_ply50.spn --depth 4 --seconds 30
```

初回実装 `24cb05f` の実測例（時間は実行環境に依存）：

| 条件 | 結果 | ノード数 | 実時間 |
| --- | --- | ---: | ---: |
| 従来・深さ3 | 深さ3以内の詰みなし | 2,064,431 | 約1.90秒 |
| 改良・深さ3 | 深さ3以内の詰みなし | 11,925 | 約0.020秒 |
| 改良・深さ4 | 深さ4以内の詰みなし | 3,053,637 | 約3.25秒 |
| 改良・深さ5 | 全78初手で深さ5以内の詰みなし | 81,659,996 | 約94.46秒 |

ノード削減には、並べ替えだけでなく厳密な点数判定による省略も含まれる。

ユーザーの成功条件をそのまま検査するには `--require-conclusion` を指定する。
詰み、または無期限の不詰みを証明した場合だけ終了コード0とする。
深さ以内の不詰み、候補探索の失敗、時間切れはいずれも終了コード1。

```bash
PYTHONPATH=. python scripts/benchmark_mate_routes.py tests/fixtures/mate_routes_bga_910749228_ply50.spn --depth 5 --seconds 180 --require-conclusion
PYTHONPATH=. python scripts/benchmark_mate_routes.py tests/fixtures/mate_routes_bga_910749228_ply50.spn --depth 7 --seconds 180 --refutation --candidates --require-conclusion
```

この初回実装の時点では、局面全体についての成功条件は未達。
上記の通常探索は深さ5以内の不詰みで終了し、反証探索は深さ7・180秒で
時間切れとなったため、成功条件付きコマンドは両方とも終了コード1となった。
`take:RR/return:W` に限定した後の局面については、開発中の反証探索で
P1勝利に至る反証を得た（P1の6行動以内、20,426,567ノード、約43.15秒）。
これは特定の初手を否定する結果であり、元局面の全78初手を否定する結果ではない。
代表手順1本だけを、全応手に対する証明とみなしてはいけない。

## 回帰検査

`tests/test_mate_route_ordering.py` で、元局面の浅い深さの結果を従来探索と比較する。
さらに、小さな局面を公開の `Game.apply()` だけで全列挙し、通常探索と反証探索、
手番の違い、補充による購入機会、貴族、最終ラウンド、同点を照合する。
入力局面の非変更と、候補探索の失敗を不詰みとしないことも検査する。
