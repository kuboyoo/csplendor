# F4前半：PR・remote CI・clean wheel受入（2026-09-06）

## 判定：BLOCKED（候補の互換性修正が必要）

[PR #26](https://github.com/kuboyoo/csplendor/pull/26)を作成し、通常push・remote CI確認・
Linux clean wheelの隔離受入まで実施した。wheel受入はPASSだが、CIは9成功・7失敗。
新しい本体/build/計測コード修正の承認はないため、原因と最小修正案を記録して停止する。
**mainへのmerge・push、常用環境更新は未実行。merge・導入は承認待ち。**

元のF1/F2/F3・lint失敗rawを保持し、今回の記録は `raw/f4_premerge_20260906/` へ追加。
索引・hash・判定は[F4 manifest](f4_premerge_manifest_20260906.json)。
F0/F1全監査、全paired A/B、実モデル受入は再実行していない。

## 候補とmain

| 対象 | 完全SHA / SHA256 |
|---|---|
| F1計測・F2受入本番コード | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| lint是正後・今回のclean wheel source／開始HEAD | `f6d28dde184ff1950c4729197a3a9ac1645b0c11` |
| fetchしたorigin/main | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` |
| ローカルmain（変更なし） | `7835f642b23251d0cb91de180006084521c74aa6` |
| candidate source digest（tests含む、lint是正後から不変） | `13d7c16e717db7e9a5f60c3e39c50746fb443c290d44d09e353cc478a5ce465e` |
| 初回CIのtest merge commit | `1dda78202fb59aa72089450844f995291858983f` |

作業対象 `/home/kuboyu/workspace/repos/csplendor-final-candidate`、branch
`review/f3-lint-correction-20260906`。開始時cleanで指定SHAに一致。
`git fetch --no-tags origin refs/heads/main:refs/remotes/origin/main` で追跡refのみ更新した。
リモートmainはF1/F3時点から進んでおらず、候補の祖先。競合解消・新たな統合は不要だった。
同名remote branchと既存PRがないことを確認して、このbranchだけを通常pushした。
auto-mergeはnull、PRはopen/unmerged。ユーザーworktreeのHEAD・status・source不変を照合した。

本番実装・build設定・依存定義・計測コード・fixtureはF1から変更なし。
今回の追加commitは報告・PR本文・検証補助・証跡のみで、clean wheelの測定対象とは区別する。
記録追加後の完全SHAは完了応答と次のコマンドで特定する：

```bash
git log -1 --format=%H -- doc/performance_experiments/f4_premerge_review_20260906.md
git rev-parse review/f3-lint-correction-20260906
```

自己SHAとそのcommitのCI結果を同じcommitへ埋め込む循環を避けるため、本書の固定rawは
初回head `f6d28dd...` に対応する。**記録追加・push後の最終headとCI結果はPR本文と完了応答へ追記し、
初回の成功を最終headへ流用しない。** ソース不変でも最終headのcheckを別に照合する。

## remote CIの対象と結果

[初回run 34012764243](https://github.com/kuboyoo/csplendor/actions/runs/34012764243)、
event=`pull_request`、head=`f6d28dde184ff1950c4729197a3a9ac1645b0c11`。
jobログのcheckout行は上表のtest merge commitとhead/base双方を含み、候補との対応を確認した。
PRの合成merge refはGitHubのCI用であり、mainへmergeしたものではない。

| 適用job | 初回結果 | 検証範囲・失敗内容 |
|---|---|---|
| test (3.9 / 3.10 / 3.11 / 3.12) | 4 SUCCESS | install、compile、lint/security lint、pytest。3.12は594 passed / 2 skipped / 4 deselected、coverage 58.89% ≥ 50% |
| test (3.8) | FAILURE | compile/lint/security lintは成功。pytestは2 failed / 592 passed / 2 skipped / 4 deselected。BooleanOptionalAction未対応 |
| native / cpp-coverage | 2 SUCCESS | native tests、C++ report-only coverage |
| package | SUCCESS | wheel/sdist/twine/auditwheel、sdist再build、隔離wheel smoke |
| strict-bindings (g++) | SUCCESS | warning-as-error build |
| Ubuntu 24.04 arm64 portable | SUCCESS | Python/native、portable wheel smoke |
| strict-bindings (clang++) | FAILURE | noble_data.h:153のconstexpr評価上限、build失敗 |
| macOS 15 arm64 portable / native | 2 FAILURE | 同じconstexpr評価上限でeditable build失敗。テスト未到達 |
| sanitizer (thread / address-undefined) | 2 FAILURE | 同じconstexpr評価上限でbuild失敗。TSan/ASan実行未到達 |
| Windows 2025 x64 portable | FAILURE | C++拡張install成功後、計測補助4テスト失敗。589 passed / 3 skipped / 4 deselected。後続native/wheel step未到達 |
| nightly-native-soak | SKIPPED（適用外） | schedule/manual専用。PRの成功数に含めない |

合計16適用job＝9 SUCCESS＋7 FAILURE、待機中なし。lint是正は成功したが、CI全体はFAILURE。
CIのskip数はローカルF3の595 passed / 1 skippedと異なるため同じ数へ書き換えない。

必須チェックの実状態は、branch情報APIで `protected=false`、
`protection.enabled=false`、required checks/contexts空、enforcement=off、適用rules APIも空。
`gh pr checks --required` は「no required checks reported」。管理用protection APIとGraphQLは403で、
そこから必須検査ゼロを推測したのではない。保護による強制がなくても、上記workflow全体を出荷gateとし、
失敗を無視してmergeしない。保護設定・CI設定・閾値・検査対象は変更していない。

## 原因・最小修正案（未実施）

1. **Clang/AppleClangの候補固有build互換性**：`src/noble_data.h:108–154`。
   `noble_mask_table_is_exact()` が5色×5値の全3125組合せを単一constant expressionで検証し、
   `constexpr evaluation hit maximum step limit` となる。該当の表・検証はmainにはない候補追加。
   GCCでの既存成功をClang成功へ読み替えない。案：同じ全組合せ・境界検査を複数の独立
   `static_assert`へ分割し、各評価量を抑える。assert削除やcompiler上限引上げで隠さない。
   要再検証：Clang/AppleClang/GCC/MSVC build、既存貴族・合法手oracle、Python/native、
   strict binding、両sanitizer、OS matrix。生成コード・表データの不変性と影響範囲の性能guardを確認。
2. **Python 3.8の計測driver互換性**：`scripts/run_phase0_baseline.py:232`。
   Python 3.9以降の`argparse.BooleanOptionalAction`を無条件使用し、baseline driver 2テスト失敗。
   driver/testはmainにない候補追加。案：`store_true/store_false`の明示的な正負flagで
   既定True・引数優先規則を維持。Python対応下限・CI matrixを緩めない。
   要再検証：Python 3.8–3.12の引数契約、既定22 pairs／slot crossover／odd-pairs拒否、関連harness tests。
3. **Windowsでの計測補助test/OS前提**：`tests/test_engine_benchmark_tools.py` のslot関連4件。
   fixtureのPOSIX executable modeがWindowsで成立せず、ELF/RPATH/SHA検査の前に
   `binary-slot rotation requires an executable file` で停止。実装は`os.fchmod`等もPOSIX依存。
   candidate追加harnessのテスト可搬性問題で、Windowsエンジンの実行不正と断定しない。
   案：OS非依存の契約検査とPOSIX実I/O検査の責務を分け、Windowsでは明示的なI/Oモデルで
   既存のinode/SHA/例外/cleanup assertionを同等に実行し、非対応実行経路の拒否契約も検証する。
   一括skip・assert削除・Windows CI除外で済ませない。具体patchは要承認。
   要再検証：全4件・関連paired harnessをLinux/Windowsで実行、Linux実fd/inode照合を維持。

以上は環境障害（runner停止・download失敗）ではなく、候補のcompiler/Python/OS互換性failure。
同じmain failureである証拠はなく、該当追加差分を確認した。mainの全CIを今回再実行はしていない。
**今回は本体・build・計測driver・既存testを一切修正しない。**

## clean wheel受入：PASS

候補SHAから新規detached worktree `/home/kuboyu/workspace/repos/csplendor-f4-wheel-20260906`
を作り、build/拡張のないclean sourceから既存PEP 517/setuptools/CMake経路でbuildした。
専用build venvへbuild依存をinstallして `pip wheel . --no-deps --no-build-isolation --no-cache-dir`
を実行。PEP 517の暗黙venvではなく、明示した専用venvで依存versionを固定観測するための
`--no-build-isolation`であり、常用環境・既存buildの流用ではない。

Python 3.12.1、GCC15.2.0、CMake4.2.3、setuptools75.8.0、pybind11 3.0.1、
wheel0.48.0、packaging26.3。runtimeはNumPy2.2.6。これらは隔離環境の実version記録であり、
repoや常用環境のdependency pinを変更していない。
portable Release、`-O3 -DNDEBUG -std=c++17 -fPIC -fvisibility=hidden`、既存pybind11
`-flto=auto -fno-fat-lto-objects`、native追加LTO OFF、PERF/VERIFY OFF、sanitizer none。
実cache/compile/link内容は `wheel_provenance.json.gz` に保存した。

| 成果物 | identity |
|---|---|
| wheel | `csplendor-0.1.0-cp312-cp312-linux_x86_64.whl`、797747 bytes |
| wheel SHA256 | `575b9b6e4e8e8b75a67a2765bde434367a388ecd88d8855884c819f9caf21258` |
| 実ロード拡張SHA256 | `f2f653f4794d5454be82bce17ee4cb0ac38f9141fc7789e4275a7d01f01bb597`（F1/F2とbyte一致） |

wheel保存先：
`/home/kuboyu/workspace/csplendor-f4-acceptance-20260906/wheels/`。
通常install先／実ロード拡張：
`/home/kuboyu/workspace/csplendor-f4-acceptance-20260906/wheel-env/lib/python3.12/site-packages/csplendor/_csplendor.cpython-312-x86_64-linux-gnu.so`。
editableは不使用。`direct_url.json`のarchive hash、wheel内とinstall後の全29 py/soファイルを照合。
実行cwdはrepo外の同隔離root `/run2`、`python -I`でPYTHONPATH・cwd・user siteを除外した。
sys.pathにsource worktreeがなく、packageとextensionがwheel-env配下であることをassertした。

- import/version、snapshot再読込み、合法手count、V3/3133 encode/decode/maskと着手・遷移：PASS。
  `simple_payment_mode=False/True`双方、seed42、各30合法ID。
- owning NumPy：float32/C連続/base=None/196特徴、legacy byte照合・遷移/破棄後保持：PASS。
- 既存fixture/testのうち7件：exact7手、5/7手frontier終局までの選択経路・訪問守備全合法応手、
  session次手番再利用、中断・再開、事前cancel、frontier状態、MCTS後の配列保持：PASS。
  legacy48 MCTSは8 simulations・batch1・合成evaluatorであり、実NN受入ではない。
- 既存test/fixtureはclean sourceのhashを確認し、必要な未装飾関数のASTだけを取得。
  assert/budgetは原文のまま、tests/scripts packageやsource側csplendorをimportしていない。
- 初回補助probeはfixture関数順の期待値を誤りFAIL。補助内の順序のみ修正し、別cwd・別rawで再実行PASS。
  元snapshot/rawを保持。本体不具合として扱わない。

## 再利用・未実施・復帰

F1 raw100＋CSV、F2/F3 raw14、lint是正raw16＝131成果物と各manifestのhashを照合。
今回の本番source不変かつ新buildの拡張もF1と同一なので、F1 paired A/B・native/reference/ASan、
F2実モデルCPU/selfplay12・17、GUI handler/frontier、session・保存再読込み・NumPyの証跡を再利用。
今回のinstalled wheelで実モデルを再loadしたとは主張しない。

未実施：失敗step以降のCI検査、PR適用外nightly soak、ブラウザ/HTTP全体、GPU、native48実NN、
実モデルA/B・棋力、Genbu（前回確認の必要file不足）、常用導入。並列MCTS累積高速化とLTO追加効果は
未確定。F1深さ7はnode上限UNKNOWNで、7手詰め完遂やAI全体速度へ外挿しない。
legacy未展開node主体のRSS増・soft deadline・利用側dirty sourceの既知制約を保持する。

修正承認・CI解消後、さらにmerge/導入の明示承認が必要。
[runbook](f4_integration_runbook_20260906.md)に従い最新base/headを再固定し、
merge後のSHAから別worktree/venvへclean install、実import identityと必要なconsumer smokeを確認する。
更新対象はユーザー指定のAI/mate worker用Pythonとroot/PYTHONPATH。モデル/config・dependency pinは別承認。
旧venv/wheel/起動先/model/config/snapshotを保持し、問題時はworkerの起動先を旧環境へ戻す。
既存session/TTを異なるbinaryへ流用しない。reset/force pushや旧成果物削除は復帰手段にしない。

**技術検査は未完了（上記3種の互換性ブロッカー）。merge・導入は承認待ち。**
