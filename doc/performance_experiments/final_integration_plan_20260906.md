# F0承認後の統合・F1累積比較：事前契約

ユーザーの「では進めてください」は、F0報告で提案した隔離統合・互換修正・回帰確認・F1までの承認。
mainへの書込み、push、PR、公開、F2〜F4、新規高速化の承認ではない。

- 基準main: `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc`（リモート再確認済み）。
- 高速化版: `9415de5766c356f9229e2bb2d22feb97d2c6b8bb`。
- 作業branch: `integration/final-candidate-20260906`。F0文書commit `f494c88`を親とし、最新mainを統合。
- 比較main: 新しいdetached worktree `csplendor-final-main`。既存main/worktreeを変更しない。
- actual merge conflictは`src/reveal_verified_solver.cpp`の1ファイル。
  mainのヒント優先をcompact/referenceの両経路へ接続し、ヒントなしの比較処理を維持する。
- 本番コードを確定・commitしてから正式計測。source/binary identityを後続文書commitと区別する。

## 正しさ・証跡再利用

F0で照合済みのPhase 6横断監査・採否と15件の証跡を再監査しない。
変更された統合treeには、mainのfrontier/情報集合/互換テスト、compact抽出の独立sort/rotate oracleを実行。
portable native全体、Python全体、reference compact OFF、hash/sidecar/rollback VERIFY、
影響範囲ASan/UBSanを実施する。並列C++実装はmainとの統合で変えていないため、
既存TSanの根拠を再利用し、通常の並列停止/sessionテストは統合回帰で確認する。
main側も同じ共通テスト範囲を確認し、baseline/candidate固有failureと環境不可を区別する。

## 計測

既存`run_paired_benchmarks.py`、fixed-slot crossover、warmup 2、22 pairs / 11 blocks、
bootstrap 10,000。1T CPU4、shared8T CPU4–11、root4T CPU4–7。計時中はbuild/testを並走しない。
同じGCC/Release/portable、PERF/VERIFYなし。mainにない内部分解microを比較しない。
共通公開APIを呼ぶ既存benchmarkの計時・fixture・checksumを再利用する薄いadapterのみ許可。
配列layout等の新内部型をmainへ移植しない。adapterと実compile/link commandを保存する。

| 役割 | workload / fixture | 固定量 |
|---|---|---|
| native primary | exact_reveal / hidden_reserve、depth7、cold | node上限1,000,000 |
| solver guards | exact_reveal / five_moves depth3 cold / depth7 warm、visible_solver / five_moves | 500,000 / 500,000 / 100,000 node上限 |
| rule guards | legal_count / legal_codes / legal_actions、midgame_250 | 各200,000回 |
| full-action guard | random_selfplay_apply / initial | 100,000手 |
| MCTS guards | legacy / hidden_reserve exact、shared 1T / five_moves exact、shared8T / hidden_reserve observable、root4T / hidden_reserve observable | 10,000 / 20,000 / 20,000 / 10,000 sim |
| V3 guard | v3_selfplay / initial | 10,000手 |
| Python primary | 既存StateFeaturizer公開経路、reachable_32_seed42 | 50,000回 |
| Python guard | 特徴量取得→full-action環境step | 50,000手 |

primaryのnative/Pythonは独立holdoutを追加する。smokeは4 pairs、採否主値にしない。
速度差が3%以上かつCI下端>1、holdoutで再現した範囲を確認された改善とする。
guardの2%超低下はCIと独立再測定で判断し、回帰を隠して一律採用しない。
新しい最適化や過去採用の無断撤去ではなく、最終レビューの判断材料として記録する。

A(main portable)→B(統合候補portable)を累積コード改善とする。
B→C(同統合候補LTO)をnative primaryだけ別計測し、A→Cも直接計測する。
各Phase倍率やA→B×B→Cの積を累積実測として使わない。
Pythonは各隔離buildと実extension path/SHAを記録し、既存pybind11 LTOを維持する。
旧/新consumerによる同じ利用側タスクを測り、native追加LTOの効果と混同しない。

S0: status/UNKNOWN理由、主手順、順序、hash/digest、論理探索統計を照合。
S1: root/tree完全一致を要求せず、実completed・unique evaluation・path steps・owner/waiter・
停止理由、ledger・VL回収を確認する。synthetic evaluatorから実NN性能・棋力は推定しない。
node上限UNKNOWNは不詰み証明・完全7手詰め時間ではない。既知5/6/7手は別の回帰guard。
全結果、CI、時間/rate、current/peak RSS、制約、失敗をdoc下へ保存し、F1で停止する。
