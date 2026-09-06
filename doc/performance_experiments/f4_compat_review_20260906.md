# F4互換性是正・再受入（2026-09-06）

## 判定：BLOCKED（元の3問題は解消、後続で別の互換性問題を検出）

[PR #26](https://github.com/kuboyoo/csplendor/pull/26)の承認済み3原因を個別commitで最小修正した。
ローカル検証、通常push、remote CI確認、修正後clean wheelの隔離受入まで実施。
既存のassert・検証集合・閾値・CI matrixは緩めていない。探索本体の別箇所や別native testへ
修正を広げず、後述の新ブロッカーを報告する。**merge・導入は承認待ち。**

旧失敗・測定rawは保持。[新manifest](f4_compat_manifest_20260906.json)と
`raw/f4_compat_20260906/` の61成果物にcommand、環境、exit code、CIログ、hashを保存した。
記録commit後の最終headに対するCIは別に照合し、PR本文／完了応答へ記録する。
最終CIを埋め込むためだけのcommitを繰り返さない。

## 固定した候補と修正

作業repoは `/home/kuboyu/workspace/repos/csplendor-final-candidate`、branchは
`review/f3-lint-correction-20260906`。開始時clean、指定HEAD/PR headに一致。
origin/mainをfetchし、F1/F3から進行していないことを確認。mainとの新しい統合なし。
同名remote branchが開始HEADのままと確認して通常pushした。main、元repoのユーザー変更は不変更。

| 対象 | 完全SHA |
|---|---|
| F1計測コード | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| 開始PR head（旧F4記録） | `fa4ea6de2f81f17acf5d83bfa0f8f6c4e35670ec` |
| 修正1：constexpr分割 | `d8ded683406d163630b96c9f7ed48c8bb75f85d6` |
| 修正2：Python 3.8引数 | `e65d1d004ddbb1d3066bbad9a77f579f2fd686c6` |
| 修正3：slot権限I/O・最終修正コード／clean wheel source | `c725b5ed33e5ba4cd4cc99d9ccee8627550b754e` |
| origin/main・PR base | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` |
| 修正コードのCI test merge | `e41c92bf28532b968a58129518c5211075832ffd` |
| ローカルmain（未変更） | `7835f642b23251d0cb91de180006084521c74aa6` |

1. `noble_data.h`：25 tuples × 125個の独立したstatic_assert評価。
   templateの各基底が隣接する `[End-25, End)` を検証し、基底0で終わる。
   3125・25の整除assertと全基底の強制instantiateで欠落を防ぎ、native testでも各tupleの訪問数1を照合。
   全table entry、全3125組、5色それぞれ255、既存MAX=4、packed requirement検証を保持。
   全集合の旧verifierの関数契約も保持するが、単一static_assertでは呼ばない。
   貴族データ・生成table・実行時mask判定のsource prefixはbyte一致。compiler上限変更なし。
2. `run_phase0_baseline.py`：store_true/store_falseを同一destへ設定。
   既定True、両flag、反復flag、両順序のlast-wins、既定22 pairs、crossover統計単位、
   odd拒否／無効化時odd許容を14テストで検証。旧defaultのassertも同じ値で維持。
3. `run_paired_benchmarks.py`：POSIX対応可否、source lstat、fd fchmodだけを小さな権限I/O境界へ抽出。
   productionでは元のlstat/fchmodを呼び、非POSIXの実slot処理は明示拒否する。
   既存4テストを「全OS参照権限モデル」と「POSIX実I/O」で同じassertのまま実行。
   モデルは権限bitのみを表現し、runner、実bytes、fd、inode、SHA、例外、cleanup、統計は代替しない。
   setuid/non-executable拒否、改竄SHA・inode置換の検出／cleanup、非対応拒否も追加した。
   Windowsで条件付けしたのはPOSIX実I/Oの8 parameter casesのみ。参照側8 casesは実行し、
   元の4件やWindows jobを一括skipしていない。Linux/macOSは両側を実行。
   **Windows上の実ELF実行・POSIX権限を検証したものではない。**

## ローカル検証

- CIと同じ `python -m ruff check --target-version py38 --select E4,E7,E9,F,W,I csplendor tests`、
  `--select S csplendor`、compileall：PASS。F2 venvのruff launcher欠落での初回失敗rawも保持し、
  既存system Pythonの同じruff 0.13.3で再実行した。lint設定や除外は変更していない。
- 関連Python：57 passed。全体 `pytest -W error --cov=csplendor --cov-report=term-missing
  --cov-fail-under=50`：617 passed / 1 skipped / 4 deselected、58.89%。
  この全体実行のin-place拡張はF1 binaryであり、後から新clean buildの拡張とのbyte一致を確認した。
- GCC15.2 portable Releaseの新規native build・ctest：44/44 PASS。全貴族組合せとchunk coverageも実行。
- Clang18.1.8／21.1.8、GCC15.2のheader単独検証は標準上限・warning-as-errorでPASS。
  Clang両versionの全体native buildは別のunused警告でFAIL。警告を無効化していない。
- 実Python3.8／3.9以降は下記remote matrixで検証。AppleClang/MSVCもremoteを使用。

## 修正コードのremote CI

[run 34014571598](https://github.com/kuboyoo/csplendor/actions/runs/34014571598)：
pull_request、completed / FAILURE。上表head/base/test mergeを全16適用jobのcheckoutログで照合。
合成test mergeはGitHub CI用であり、mainへmergeしたものではない。

| 適用job | 結果・実行範囲 |
|---|---|
| test Python 3.8 / 3.9 / 3.10 / 3.11 / 3.12 | 5 SUCCESS。全て616 passed / 2 skipped / 4 deselected。lint/security/compile成功、3.12 coverage58.89% ≥50% |
| native / cpp-coverage | 2 SUCCESS。44件実行、C++ coverageは既存report-only |
| package | SUCCESS。wheel/sdist/twine/auditwheel、sdist再build、通常installのwheel smoke |
| strict-bindings g++ | SUCCESS |
| Ubuntu 24.04 arm64 portable | SUCCESS。Python616件、native44件、wheel build/install/smokeすべて成功 |
| strict-bindings clang++ | FAILURE。constexprは解消、下記unused警告 |
| sanitizer thread / address-undefined | 2 FAILURE。unused警告でbuild停止。TSan/ASan/UBSan実検査は未実施 |
| macOS 15 arm64 portable / native | 2 FAILURE。AppleClang拡張buildは成功。各615 passed / 1 failed / 2 skipped / 4 deselected。既存timeout testのGNU time前提で失敗、native/wheel未到達 |
| Windows 2025 x64 portable | FAILURE。MSVC拡張build・Python607 passed / 11 skipped / 4 deselectedは成功。後続native buildで既存テストのC4244/C4267。native実行・wheel build/install/smokeは未到達 |
| nightly-native-soak | SKIPPED（schedule/manual専用、PR適用外） |

16適用job＝10 SUCCESS＋6 FAILURE、適用外skip1。failed stepに続くskipは未実施として扱う。
Windowsの11 skipsは従来3＋POSIX実I/O8で、OS共通の参照契約は通過している。
branch情報APIはprotected=false、required checks空、適用rules空。強制がなくても全workflowをgateとし、
失敗を免除しない。CI設定・matrix・依存定義・coverage基準・warning設定は不変更。

## 修正後clean wheel：PASS

clean detached source：`/home/kuboyu/workspace/repos/csplendor-f4-compat-wheel-20260906`（c725b5e）。
新build venv＋既存手順 `pip wheel . --no-deps --no-build-isolation --no-cache-dir`。
新wheel venvへ通常installし、repo外の新規cwd `/home/kuboyu/workspace/csplendor-f4-compat-20260906/run`
から `python -I` で既存[F4 probe](f4_wheel_probe_20260906.py)を実行した。

- wheel：`/home/kuboyu/workspace/csplendor-f4-compat-20260906/wheels/csplendor-0.1.0-cp312-cp312-linux_x86_64.whl`、797860 bytes。
- wheel SHA256：`e577b9c124c9961cb4492ea50bcd9910a1d6e91778b9ae6097c9429aa4166654`。
- 実ロード先：`/home/kuboyu/workspace/csplendor-f4-compat-20260906/wheel-env/lib/python3.12/site-packages/csplendor/_csplendor.cpython-312-x86_64-linux-gnu.so`。
- extension SHA256：`f2f653f4794d5454be82bce17ee4cb0ac38f9141fc7789e4275a7d01f01bb597`、F1/F2とbyte一致。
- source digest（tests含む）：`5e2902db59dbda49754d651d915b080043bbb0abe3c05b7054fa6874d8c37c81`。
  旧lint/F4は`13d7c16e717db7e9a5f60c3e39c50746fb443c290d44d09e353cc478a5ce465e`。
- Python3.12.1、GCC15.2.0、CMake4.2.3、pybind11 3.0.1、setuptools75.8.0、wheel0.48.0、
  packaging26.3、NumPy2.2.6。portable Release、native追加LTO OFF、PERF/VERIFY OFF、sanitizer none。
  CMake cacheは旧buildと一致、compile/linkは新root置換以外一致。既存pybind11 LTOは保持。

import/通常install identity、全29 py/so一致、両payment mode、合法手・着手・V3/3133、
owning NumPy196特徴、snapshot保存再読込み、既存7ケース（exact7、5/7 frontier、session再利用、
中断/再開、事前cancel、MCTS後配列所有権）はPASS。sys.pathにrepoなし。
MCTSはlegacy48・8 simulations・batch1・合成evaluatorであり、今回の実NN受入ではない。

## F1/F2再利用・性能guard

**F1から本体headerと計測driverに差分がある。source不変とは記載しない。**
一方、データ・table生成・実行時mask判定のsource prefixと、同条件の新拡張全byte、
installed全29 py/soが一致した。build/依存と計測fixture、runnerのtiming・順序・統計関数のASTも不変。
この範囲に限定してF1実測、F2実モデルCPU/GUI handler/session等の既存証跡を再利用する。
新wheelの実モデル再load、Clang/Windowsの成功、sanitizer完了を旧証跡で代用しない。

追加guardは既存F1 ELFをA/Aとして新runnerで4 pairs/2 crossover blocks、合法手fixtureを実行。
実fd/inode固定、pre/post SHA、ABBA割当、semantic digest、cleanup、統計metadataを照合してPASS。
これはrunnerの実I/O回帰検査で、エンジン高速化のA/Bや新性能倍率ではない。
byte同一のエンジンに全F1再測定は行わなかった。
旧wheelとの差はMETADATA内のf6→fa4 README進捗とそのRECORD、archive timestamp。
依存metadata headerは一致。今回の新最適化やdependency pin変更ではない。

## 新ブロッカーと必要な次の承認

元の3原因以外への修正は行っていない。新たな探索意味論の不一致は観測していないが、次を解消するまで出荷不可。

1. Clang：`solver_path.h:58`のPERF OFF時unused-but-set `comparisons`、
   `solver_action_filter.h:50,64`のPreferHint=false時unused capture。
   いずれもF1から不変のコードで、constexpr失敗の後ろに隠れていた。
   案：計測専用変数の宣言/更新を計測条件に揃え、hint特殊化で不要なcaptureを生成しない。
   要検証：PERF ON/OFF、hint両値の固定順oracle、Clang strict、両sanitizerの実検査、binary/影響性能guard。
2. macOS：既存timeout testが`/usr/bin/time -f`をGNUと仮定し、BSD timeで起動失敗。
   `_run_with_rss`と元テストの動作は今回不変更。
   案：GNU time能力とprocess-group検査の境界を分け、LinuxのRSS/affinity契約を保持した上で
   非GNU環境のtimeout/cleanup検査を用意する。要検証：GNU/BSD/不在・実process cleanupとmacOS後続native/wheel。
3. Windows：`state_feature_table_unit.cpp:10,20,22,37,39`のint→int8_t、
   `action_selection_unit.cpp:82,85`のsize_t→uint16_t。両テストはF1から不変。
   案：境界値集合を変えず型を明示し、index範囲を検査した上で変換する。
   要検証：元のbitwise/selection全ケース、MSVC native実行、後続wheel build/install/smoke。

未実施：上記失敗の後続検査、nightly、GPU/ブラウザ全体/実モデルA/B・棋力、Genbu不足file、常用導入。
F1のdepth7はnode-limit UNKNOWN、AI全体/7手完遂速度へ外挿しない。
並列MCTS累積倍率・追加LTO効果は未確定。既知RSS増・soft deadline・利用側dirty sourceの制約を保持。

復帰・承認後導入は[runbook](f4_integration_runbook_20260906.md)。旧venv/wheel/model/configを保持し、
起動Pythonとroot/PYTHONPATHを旧環境へ戻す。session/TTを異なるbinaryへ流用しない。
mainへのmerge/push、常用環境更新、tag/Release/PyPI公開、branch/worktree削除は未実行。

記録commitの特定：`git log -1 --format=%H -- doc/performance_experiments/f4_compat_review_20260906.md`。
この記録commitと、上表の修正コード／wheel sourceを区別する。**merge・導入は承認待ち。**
