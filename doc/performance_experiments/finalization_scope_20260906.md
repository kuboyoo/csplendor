# F0：最終候補・最新mainとの統合範囲

この文書は承認前のF0記録。後続の承認・統合・F1結果は
[最終候補との累積比較](final_main_vs_candidate_20260906.md)を参照。
以下の当時の判断とmanifestは履歴として変更しない。

## 結論：BLOCKED（統合承認待ち、F0で停止）

**最新mainには別系統が既に取り込まれている。高速化版単体を、そのまま最終出荷候補には固定しない。**
2026-09-06の`git ls-remote`で再確認したmainは、依頼書の参照から進んでいた。
PR #23のmerge commit `f5ec6c5`は`595d588`を含み、両者のGit treeも完全一致する。
一方、Phase 6完了版には、その公開API・frontier修正・Python互換性修正が含まれない。
mainの既存機能を保持するには統合が必要であり、承認前のmerge・手動移植・正式F1計測は行わない。

今回は隔離worktree `csplendor-finalization` / branch `docs/finalization-scope-20260906`で
記録のみ追加した。エンジン、既存harness、main、既存作業branch、ユーザーの未コミット変更は変更しない。
push・PR・公開も未実施。4A保留と過去の棄却判断を維持する。

## 固定した参照と関係

| 参照 | SHA | 今回の扱い |
|---|---|---|
| BASE_MAIN_SHA（リモート最新main） | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` | 今回観測した基準。未計測 |
| CANDIDATE_SHA（perf/build-profiles） | `9415de5766c356f9229e2bb2d22feb97d2c6b8bb` | 高速化版単体の不変参照。最終出荷候補への確定は保留 |
| 別系統 codex-mate-frontier-fixes-20260906 | `595d58805a99ee0c48586d04ad23ab14b459750d` | 最新mainに統合済み、高速化版には未統合 |
| 共通祖先・旧依頼書基準 | `7835f642b23251d0cb91de180006084521c74aa6` | 最新mainの代わりに比較基準へ使用しない |

```text
7835f64 ── 高速化30 commits ───────── 9415de5  perf/build-profiles
    └──── 別系統8 commits ── 595d588 ── f5ec6c5  リモートmain（PR #23）
```

main対高速化版のmerge-baseは`7835f64`、固有commit数はmain側9 / 高速化側30。
main対別系統のmerge-baseは`595d588`。
ローカル`main`と`origin/main`はともに`7835f64`のまま維持した。
リモートの新SHAを`git fetch --no-write-fetch-head --no-tags --no-recurse-submodules origin <SHA>`で
objectとして取得しただけで、これらのref、既存worktreeのHEADや内容は更新していない。

source digestはPhase 3D-1/6と同じ方式を使用：root CMakeLists.txtと
src/scripts/tests/csplendor内の`.h/.cpp/.py/.txt/.json`をソートし、
各file SHA-256、空白2個、相対path、LFの連結をSHA-256化した。
この限定digestに含まれないsetup/packaging/CIはrawに別hash、全tracked treeにはGit tree IDを記録する。

- 最新main / 別系統：`1256c8b63288c41bb73b971e6852e112fe31ef6c4858660ccdee6aee8e6a930c`
- 高速化版：`763d84418fcedaebc363f79a7a1e2f61dbc26dffcb5c0ca16cbf0843e6567752`
- FINAL_CANDIDATE_SHA：**未確定（manifestではnull）**。報告追加commitを新たな検証済みengineと扱わない。

## 差分の分類と必要な統合

共通祖先から最新mainまで79ファイル、高速化版まで2,602ファイルが変更されている。
R0記録58ファイルは両系統でblob一致し、重複取り込みや再監査は不要。
残るmain側の非重複差分は**21ファイル**。完全な一覧・patch・commit別履歴をrawへ保存した。
最新main対高速化版の直接差分2,560ファイルのうち2,488はdoc（圧縮証跡を含む）で、
このファイル数を本番コード変更量と読み替えない。

| 範囲 | 主なfile / commit | 必要性と対応境界 |
|---|---|---|
| 遅延frontier公開経路（3ファイル） | `csplendor/mate_frontier.py`, `src/bindings_solvers.cpp`, `tests/test_reveal_verified_solver.py` / `5f29b50`, `595d588` | `preferred_attacker_actions`の受け渡しと既定node予算1,500,000→5,000,000、広い守備応手・ヒントの回帰テストを保持する。予算差を同条件の速度向上に数えない |
| ヒント保持（1ファイル＋上記テスト） | `src/reveal_verified_solver.cpp` / `595d588` | take/reserveを上限で切る**前**にヒントを優先する修正。高速化版のcompact経路への接続が必要（下記） |
| 情報集合キー（8ファイル） | `src/information_state.{h,cpp}`, `src/bindings_rules.cpp`, root/testsの`CMakeLists.txt`, `tests/information_state_unit.cpp`, `tests/test_information_state.py`, `doc/information_state.md` / `a686973`, `3d9dcf2`, `595d588` | 最新mainの公開APIを保持。format v2、USI未知購入枚数、observer-safeな識別、ARM64向けテストを含む。高速化とは別の機能差分 |
| Python互換・既存CI修正（6ファイル） | `csplendor/{__init__,mate_depth,mate_session}.py`, `.github/workflows/ci.yml`, `tests/test_api_application_boundaries.py`, `tests/test_generate_mate_puzzles.py` / `ae99587`, `0e1c2bc`, `2df4dc5` | Python 3.8で使えない`shutdown(cancel_futures=True)`を残さない。明示future cancelと待機、例外処理、import/format、macOSの重複検査除去を保持 |
| 文書・契約（3ファイル） | `doc/api_ref.md`, `doc/index.md`, `doc/refactoring_contracts.json` | 情報集合APIと高速化側NumPy API、standalone headerリストを双方保持 |

両系統が同じファイルを別々に変更しているのは、重複R0を除くと
`CMakeLists.txt`, `tests/CMakeLists.txt`, `src/reveal_verified_solver.cpp`,
`doc/api_ref.md`, `doc/refactoring_contracts.json`の5ファイル。
これは**同時変更の静的照合**であり、mergeを試した結果のconflict一覧ではない。

### 特にヒント修正は旧vector経路だけの移植では足りない

高速化版の`forced_attacker_actions`は、既定ONの
`CSPLENDOR_COMPACT_FORCED_ACTIONS`で`src/solver_action_filter.h`の
`compact_forced_attacker_actions`を呼ぶ。この関数はbounded配列から上限内の候補だけを返し、
その後で`prefer_candidate_action(filtered, depth)`を実行する。
既に捨てられたヒントは、この後処理では復活できない。現行helperにはヒント引数もない。

mainの`595d588`は旧vector実装のtake/reserve列を切り詰める前にヒントを優先する。
そのpatchを高速化版のfallbackだけへ適用すると、既定compact経路に修正が届かない。
承認後はcompact/reference**両経路**でmainの意味を保持し、追加回帰
`test_reveal_verified_frontier_keeps_hint_beyond_reserve_pruning_limit`等で照合する必要がある。
当該helperの調整は上記21ファイルに加えて必要になり得るが、今回は実装していない。
これは統合上の正しさ修正であり、新しい枝刈り・探索方針や高速化案の導入ではない。

「高速化branch単体のみ出荷し、mainの修正は除外する」とは承認されていない。
特に最新mainの公開APIを失う状態を最終候補と呼べないため、比較可能な旧局面だけを選んで
正式計測を先行することも避けた。**別系統をもう一度丸ごとmergeする提案ではなく、
最新mainと採用済み高速化を隔離した統合候補に両立させる承認を求める。**

## 既存証跡の再利用範囲

[Phase 6報告](phase6_build_profiles_20260906.md)、[共通監査](phase6_common_audit_20260906.md)、
[5C-B報告](phase5cb_features_20260906.md)、および関係する旧依頼書・実装差分を確認した。
採用/棄却/未着手/保留一覧は共通監査を正とし、再作成していない。
各milestone commitが`9415de5`の祖先であること、現在の候補source/file hashが
Phase 6最終source記録と一致することを確認した。採用コードの復活・撤去・変更なし。

[Phase 6 manifest](phase6_evidence_20260906.json)と、そのうち今回参照する**15件**の
final source・テスト・LTO記録についてSHAを照合し、既存ログを読み直した。
過去2,162件の横断SHA監査は、その通過記録を再利用しており**再実行していない**。

- native portable / LTO各42件、Python586 passed / 1 skipped / 4 deselected、性能4件。
- ASan/UBSan 8 suites、TSan 4 suites、強いVERIFY oracle 3 suites。
- wheel build・実extensionの明示import、Python compile。
- 5C-Bまでと最終portable binaryの一致、同じsourceに対するLTO正式/holdout/最終記録。

以上は**高速化版単体**の既存検証であり、最新版mainとの統合テストではない。
既知5/6/7手、frontier、session等の既存テストが通っていても、新たなmainのヒント回帰まで
検証したという意味にはならない。今回はengineのbuild・全テスト・sanitizerを繰り返さなかった。
今回追加した記録scriptはpy_compileと実行時のsource/ancestry/証跡/保護対象不変assertで確認した。

既知tradeoffも維持：LTOはsolver向けopt-in、MCTSには低下傾向、legacy大量未展開nodeの
RSS増、cacheはsolve前後trimでhard RSS capではない、実NN・Apple/Windows実行は未検証。
hardware perf不可の過去記録も未検証環境のPASSへ読み替えない。

## F1の未実施事項と再開条件

| 項目 | 今回の状態 |
|---|---|
| 最新main portable → 最終候補 portableの累積A/B | 未実施：統合範囲承認待ち |
| 累積speedup・95% block CI・RSS | 未取得。Phase別倍率の積やPhase 6の値で代用しない |
| S0 semantic digest / S1 ledgerの累積照合 | 未実施。今回確認したのはsource/APIの差分と既存証跡 |
| baseline固有 / candidate固有runtime failure | 未評価。今回の停止原因は静的に確認した統合不足 |
| F2実利用受入・F3 PR準備・F4公開/統合 | 未着手、今回自動では進めない |

承認後の範囲案：新しい隔離統合branchで、最新mainの上記修正と採用済み高速化を保持する。
compact/referenceのヒント契約、Python 3.8互換、情報集合v2とhidden/USI、frontier/sessionを
影響範囲として回帰確認し、変更後source/binaryで最終候補を固定する。
mainが再び進んだ場合は、その時点のSHA・差分を先に確認する。

その後のF1では既存paired runner / fixed-slot crossover / 22 pairs・11 blocks /
block bootstrapとprimary holdoutを用いる。A(main portable)→B(統合候補portable)の
コード改善とB→C(同候補opt-in LTO、solverのみ)のbuild効果を分離する。
A→Cを出す場合も直接比較する。Python拡張の既存LTOはnative追加LTOとは別扱い。
mainにないharness/APIは共通タスクの薄いadapterを検討し、engine移植でreferenceを変えない。
S0は意味/順序/論理統計、S1は実completed・unique evaluation・ledger・VL回収を照合する。
ヒントなし等の共通局面と挙動が変わる回帰fixtureを分け、旧bug再現を同値性gateにしない。

今回のPhase 6 LTO rawは候補sourceが不変なら再利用できるが、統合によりcode/binaryが変わった
後のLTO効果を保証しない。統合後に必要な代表sliceだけ再測定する。
保留4A、棄却PGO/Linux native等の再導入は行わない。

## 記録

- [manifest](finalization_scope_manifest_20260906.json)：SHA/source digest、採用継承、除外・未確定範囲、未計測項目。
- [圧縮raw](raw/finalization_20260906/scope_inventory.json.gz)：3参照の全source/file hash、差分一覧、21ファイルのpatch、履歴、重複判定、保護worktreeの照合、再利用した証跡。
- [記録script](finalization_scope_record_20260906.py)：計測/merge/pushを行わない。出力は既存fileを上書きせず、再実行時は停止する。

rawのSHA-256：`6021da723f717a6031ab914a51355e172f9d0e6927c43108ddbdc501c92bead1`。
報告はdoc下に保存し、ローカルの文書commitとengineのCANDIDATE_SHAを区別する。
**次の作業は統合範囲の明示承認後のみ。今回の状態はF0 / BLOCKED。**
