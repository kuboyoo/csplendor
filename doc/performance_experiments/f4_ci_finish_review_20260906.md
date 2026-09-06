# F4前半：CI互換性仕上げ（2026-09-06）

## 判定：READY_FOR_REVIEW（merge・導入は未承認）

最終コードのremote CIは16適用jobsが全て成功し、clean wheel受入もPASS。
[manifest](f4_ci_finish_manifest_20260906.json)と147件の新rawに保存した。
記録commit後の最終headに対するCIは[PR #26](https://github.com/kuboyoo/csplendor/pull/26)に別途照合・記録する。
**mainへのmerge・導入は承認待ち。** 新しい高速化、CI緩和、常用環境更新は行っていない。

作業repoは `/home/kuboyu/workspace/repos/csplendor-final-candidate`、branchは
`review/f3-lint-correction-20260906`。開始時cleanで指定headとPRが一致。
AGENTS、前回報告・manifest、runbook、実CIログを確認し、前回61成果物のhashを照合した。
origin/mainはF1/F3から進行していない。元repoのユーザー変更、ローカルmain、旧失敗rawを保持した。

| identity | 完全SHA / SHA256 |
|---|---|
| F1計測コード（旧binaryの測定） | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| 開始PR head（前回記録） | `da73c5ddfb4cd4da6a1525e4b362c48fe24b6df1` |
| 今回の最終コード・clean wheel source | `8b6dd8b48526dfa1eda8ccbc00a9355f5abc8cdb` |
| origin/main・PR base | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` |
| 最終コードCI test merge | `ff3994071ab3656a0cc860b41ea7c4a266c68ee3` |
| testsを含む最終source digest | `cdc20fe72092271f8d37c21c1320f93d2bf9eab64b7eb73be28665871e36decd` |
| ローカルmain（未変更） | `7835f642b23251d0cb91de180006084521c74aa6` |

この後の記録commitはdocのみ。最終PR head/test mergeとそのCIはPR本文に追記し、
CI結果を書き込むためだけのcommitを反復しない。合成test mergeはmainへの実mergeではない。

## 原因別の修正と維持した契約

| 原因 | commit | 最小変更 |
|---|---|---|
| Clang unused変数/capture | `e4d1464c34dc3d137103a3576efe996ae9313168` | comparisonsの宣言・更新・参照をPERF条件へ統一。hintなし比較器はcaptureなし、hintありは同じ基礎比較へ委譲 |
| MSVC縮小変換 | `be9fcaa21bdeefbae8f54debd3bc6065f6f26dfe` | int8/uint16表現可能性を確認して明示変換 |
| macOS GNU time前提 | `3c8921c06b47d48edd558c0c807890ab3f3485dc` | disposable PythonでGNU flags/RSS形式を事前判定。対象自体は一度だけ実行 |
| TSanのnew/delete重複 | `c1a8cb5506dab2b8e885796cfba1bd58f5160b93` | TSan時は公式allocator hookで無割当検査を維持。監視が動く陽性対照も追加 |
| Darwin zombie / Linux proc消滅 | `cdba39779fbfa18a5756d14bf27fd2a5823ca764` | leaderをpoll/reapし、group消滅が確認できたEPERMのみ許容。proc読取りを単一操作にする |
| GCC陽性対照の警告 | `404ddeb48ca793e7f74559a893031a655984d058` | volatile関数pointerで実割当を観測。正当なmalloc-backed replacement deleteのinline診断を避ける |
| proc読取り時ESRCH | `8b6dd8b48526dfa1eda8ccbc00a9355f5abc8cdb` | ENOENTに加えてESRCHを終了として扱い、両経路をモデル検査 |

- 比較規則、カテゴリ順序、truncate上限、hint rotate-before-truncate規則は不変。
  PreferHint=falseはClang/GCC双方でcaptureなしをstatic_assertし、true/falseを同じfull-sort oracleで検査。
  path counterはfind 4、LIFO比較合計5（PERF ON）、OFFでは全0を検査。
- invalid card ID `-128,-2,90,127`、observer `-1,0,1`、全card、MAX_MOVES、UINT16_MAXの
  元のassertを保持。表現可能性はゲーム上の合法性ではなく、不正値をoracleから除外していない。
- GNU timeの対応・非対応・不在・異常format・probe timeoutを検査。対応しなければRSSはNone。
  GNU `%M`のKiB、targetの計時範囲、affinity、A/B/slot crossover、統計、例外は維持。
  targetの失敗/timeoutを能力失敗として再実行しない。Linux実fd/inode/SHAを維持。
- 非GNU経路でも実targetと子プロセスを起動し、SIGTERMを無視する子の停止と親の回収を検査。
  macOSは/procがないためpsでも子の消滅/終了を検査。Windowsは元から非対応のPOSIX実slot/groupだけ
  条件付けし、参照モデル・非対応拒否・実direct-child timeoutを実行する。実ELF検証済みとはしない。
- Darwinのkillpgはzombieを除外してEPERMを返し得る
  （[Apple XNU killpg1](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_sig.c)）。
  PermissionErrorを一括無視せず、reap後もgroupが存在すれば例外を維持する回帰テストを追加。
  Linuxではprocのexists→read間だけでなくread中にもENOENT/ESRCHとなるため、両方だけを扱う。
- TSanは`sanitizer/allocator_interface.h`の登録hookをmain開始時に導入。元のrollback無割当assertは残る。
  mallocも観測するので検査を弱めていない。非TSanのreplacement new/deleteは保持。
  GCC警告はこのreplacementのfreeをinlineした陽性対照への診断で、実allocation/freeの不一致ではない。
  関数pointerのvolatile読出しは観測用割当の最適化除去も防ぎ、counter増加を全buildで要求する。

前回採用のconstexpr分割（全3125組・table entry・境界）、Python3.8 flag契約、POSIX I/O分離は不変更。
warning設定、assert、skip対象の一括拡大、対応OS/Python、CI閾値、依存定義は変更していない。

## ローカル検証

- Clang18 strict（`-Wall -Wextra -Wpedantic -Werror`）拡張＋native build、44/44実行PASS。
  PERF ONのsolver/components・counter・型変換関連4件もPASS。GCC13.4の新native build・44/44 PASS。
- Clang18 ASan/UBSan、TSanとも44/44を実行PASS。最後の陽性対照変更は両sanitizerで再build・再実行PASS。
  buildだけをsanitizer PASSと扱っていない。実メモリ不正・UB・raceは検出されていない。
- 関連Python77件、CIと同じruff lint/securityとcompileall PASS。
  Python3.12全体は最終testsで637 passed / 1 skipped / 4 deselected、coverage **58.89% ≥50%**。
  新wheelの拡張を専用test venvから親・spawn子の両方で読み込ませ、旧in-place拡張の混入を防いだ。
  coverage runでpackage importから測定し、元のpytest設定・警告error・収集対象・50% gateを維持。
  hosted CIでは従来のpytest-covコマンドそのものを使う。
- ローカル試行の失敗もrawに保存：最初のcapture案はClangで通ってGCCのempty検査で不適合だったため
  両compilerでemptyとなる比較器に是正。TSanの初回リンク重複を保存。
  wheel全体test用ラッパーの初回はspawn子のimport先不一致で停止したため、ラッパーだけを是正し全体再実行。
  これらをエンジンの探索意味論failureや成功へ読み替えていない。

## 修正pushとリモートCIの経過

| run | code head | 結果・独立原因 |
|---|---|---|
| [34015102420](https://github.com/kuboyoo/csplendor/actions/runs/34015102420)（開始） | da73c5d | 10成功/6失敗/適用外1。既知Clang、GNU time、MSVCを実ログで確認 |
| [34016455084](https://github.com/kuboyoo/csplendor/actions/runs/34016455084)（1回目） | 3c8921c | 12成功/4失敗/適用外1。TSanリンク重複、macOS EPERMと派生する未回収pipe警告、arm64 proc ENOENT。Windowsはnative・適用wheelまで成功 |
| [34016913244](https://github.com/kuboyoo/csplendor/actions/runs/34016913244)（2回目） | cdba397 | 13成功/3失敗/適用外1。GCCの陽性対照警告（native/arm64）、proc ESRCH（Python3.10）。両sanitizer44実行・macOS両profile・Windows後続wheelは成功 |
| [34017353409](https://github.com/kuboyoo/csplendor/actions/runs/34017353409)（3回目） | 8b6dd8b | **16成功/失敗0/適用外1。全適用jobのcheckoutを照合済み** |

各runの全適用jobについて、実checkoutログのhead/base/test mergeを照合する。
旧headの成功を現headへ流用せず、失敗依存の未到達stepとprofile条件による適用外skipを区別する。
branch APIはprotected=false、required checks空、rules空だが、全workflowを出荷gateとして扱う。

最終コードCIの実行範囲：

| job | 確認結果 |
|---|---|
| Python3.8–3.12 | 各636 passed / 2 skipped / 4 deselected。lint/security/compile PASS、3.12 coverage58.89% |
| native / cpp-coverage | 各44/44実行PASS。C++ coverageは従来のreport-only |
| strict-bindings GCC / Clang | 両方PASS。warning-as-error維持 |
| ASan/UBSan / TSan | 両方44/44実行PASS。対応しない他方のsanitizer stepだけ適用外skip |
| Linux arm64 portable | Python636 passed、native44/44、wheel build/install/smoke PASS |
| macOS arm64 portable | Python635 passed / 3 skipped、native44/44、arm64 wheel architecture/build/install/smoke PASS |
| macOS arm64 native | Python635 passed / 3 skipped、native44/44 PASS。wheel stepはprofile上の適用外 |
| Windows x64 portable | Python625 passed / 13 skipped、native44/44、wheel build/install/smoke PASS |
| package | wheel/sdist/twine/auditwheel、sdistから再build、隔離install/smoke PASS |
| nightly-native-soak | PR適用外skip。実行済みとはしない |

macOSのGNU time実RSSテスト1件だけは適用外。能力モデル、非GNUの実起動/timeout/group回収は実行済み。
WindowsもPOSIX実I/O8件・process group2件・GNU time実RSS1件が追加の適用外で、共通契約は実行。
既存2 skipとperformance markerの4 deselectedを変更していない。失敗依存で未到達の必須stepはない。
既存2 skipは生成済み問題データと隣接するUSI正本checkoutの不在によるもの。
ローカルにはUSI checkoutがあるため、全体は637 passed / 1 skippedとなる。

## F1/F2との差分、性能guard、再利用範囲

F1から本体は`noble_data.h`、`solver_action_filter.h`、`solver_path.h`に差分あり。
計測driverは`run_phase0_baseline.py`と`run_paired_benchmarks.py`に差分あり。
本体・計測コード不変とは記載しない。build/依存/CI設定、Python28ファイル、既存計測fixtureは不変。
貴族データ・生成table・runtime prefixは旧検証hashと一致し、全境界のnative検査を維持した。
runnerの旧45 top-level関数ASTは不変。変更したのはRSS/cleanupのI/O境界と追加probeのみ。

同条件GCC15.2 buildで、旧拡張 `f2f653f4…` と新拡張 `71149823…` は**byte不一致**。
native benchmarkと拡張の.textも不一致。28 Pythonファイルとwheel metadata/dependency headerは一致。
バイナリ不変を根拠にF1/F2を丸ごと再利用せず、以下のsolver限定guardと新wheel受入を実施した。

既存F1 common harness・保存済みF1候補binaryをreferenceに、新portable/LTO OFF binaryと直接paired比較。
各22 pairs/11二対crossover blocks、ABBA、2 warmups、bootstrap10000、CPU4固定。
statistical/semantic gateは既存runnerを使用。非同期MCTSのtree一致を要求する比較ではない。

| 固定順solver guard | B/A速度比 [95% block CI] | A/B current RSS中央値 KiB | A/B max RSS中央値 KiB |
|---|---|---|---|
| exact_reveal / hidden_reserve / depth7 / 100万nodes | 1.0103 [0.9981, 1.0269] | 49,186 / 49,204 | 54,190 / 54,128 |
| visible_solver / five_moves / 10万nodes | 1.0192 [0.9973, 1.0444] | 11,270 / 11,282 | 11,490 / 11,524 |

両者のsemantic digest・正しさcounterは全pair一致（`e5c83af438331d73` / `b6cdbdc80156e463`）。
中央値2%超の低下は検出されず、追加高速化を主張する結果でもない。
exact depth7はnode-limit **UNKNOWN**であり7手詰め完遂時間ではない。
最終runnerでもLinux実ELF A/A4 pairsを実行し、slot inode、stage/post SHA、cleanup、RSS、統計契約を照合。

F1のmain対旧候補倍率は旧binaryの実測として保持し、このguardと乗算しない。
F2の実モデル/GUI証跡は旧binaryでの結果として保持。今回の実モデル再loadは未実施で、現binaryのPASSとはしない。
変更がないPython bridge、特徴量・評価API・fixture・build条件は既存証跡を参照し、変更したsolverは新テストで確認。
全F1、全実モデル検証、新規学習は繰り返していない。並列MCTS累積倍率・追加LTO効果は引き続き未確定。

## 最終clean wheelの隔離受入

- source：`/home/kuboyu/workspace/repos/csplendor-f4-ci-finish-wheel3-20260906`、上表8b6dd8bのclean worktree。
- build：Python3.12.1、GCC15.2、setuptools75.8.0、pybind11 3.0.1、CMake4.2.3、wheel0.48.0、packaging26.3。
  portable Release、追加LTO OFF、PERF/VERIFY OFF、sanitizerなし。既存pybind11 LTOは従来どおり。
  compiler flags/link/cacheは前回とroot path以外一致。依存pinを変更したのではなく隔離build条件を再現した。
- wheel：`/home/kuboyu/workspace/csplendor-f4-ci-finish-20260906/wheels-cycle3/csplendor-0.1.0-cp312-cp312-linux_x86_64.whl`
  （797,767 bytes）、SHA256 `f4fa6dd4965ad7cbafb3a1407fdf47c5bb044789cc6125ecd8b7934772140f09`。
- 通常install先：同rootの`wheel-cycle3-env`（NumPy2.2.6）。editable不使用。
  repo外`acceptance-cycle3`から`python -I`で実行し、sys.pathにrepoがないことを確認。
- 実ロード先：`/home/kuboyu/workspace/csplendor-f4-ci-finish-20260906/wheel-cycle3-env/lib/python3.12/site-packages/csplendor/_csplendor.cpython-312-x86_64-linux-gnu.so`
  SHA256 `711498238be2a8e61148ce210bcb5be162c65e9950f834f1b635b9fb1833cd06`。
- import、合法手・着手、両payment mode、V3/3133 mask、owning NumPy196、snapshot保存・再読込み、
  既存7ケース（exact7、5/7 frontier、session再利用、中断/再開、NumPy寿命）はPASS。
  MCTS smokeはlegacy48・8 simulations・batch1のsynthetic evaluatorであり実NNではない。
  全installed29 py/soをwheel memberと照合。今回3回のclean buildも実行packageは同一、wheel hash差はzip記録差。

## 残る制約・未実行操作

GPU、ブラウザ描画、現binaryでの実モデル再load/棋力、nightly soak、全F1再計測は未実施。
Windows実ELF/POSIX slotとmacOSのGNU RSSは非対応として明示し、共通契約と実timeout検査で代替した。
利用側dirty tree、常用Python/dependency pinの選択、legacy MCTS RSS tradeoff、soft deadline等の既知制約は維持。

明示承認後だけ最新main/headを再確認し、PR review/CIを経て統合する。mainが進めば別途差分確認が必要。
merge SHAから新worktree/venvでclean wheelを作り、GUIのAI/mate Python・root/PYTHONPATHと
利用側pinを承認された対象だけ更新して小さな実利用受入を行う。旧venv/wheel/model/configは保持し、
問題時は起動先を旧環境へ戻す。reset/force pushによる復帰はしない。
[統合・復帰runbook](f4_integration_runbook_20260906.md)の導入操作は未実行。

mainへのmerge/直接push、自動merge、常用環境更新、dependency pin変更、tag/Release/PyPI公開、
branch/worktree削除は行っていない。**merge・導入は承認待ち。**
