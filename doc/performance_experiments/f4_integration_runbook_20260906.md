# F4統合・導入runbook：merge・導入は承認待ち

2026-09-06追記。F3 lint是正は完了。承認されたF4前半（作業branch push・PR作成・
remote CI・隔離clean wheel受入）を実施した。wheelはPASS、CIで互換性failureが残り **BLOCKED**。
[PR #26](https://github.com/kuboyoo/csplendor/pull/26)と[前半報告](f4_premerge_review_20260906.md)を参照。
以下のmainへの統合・常用環境更新はまだ承認されていない。

## 固定するref

- F4前半でfetchしたorigin/main：`f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc`、F1/F3から進行なし。
- ローカルmain：`7835f642b23251d0cb91de180006084521c74aa6`、未変更。追跡refのみ更新した。
- 検証済みcode：`b202e6a0cbb2eded9bc2ee5e59f750428e73ca49`。
- F1記録：`49878b661298bf45e39c5f5ca5afa6d0e363736a`。
- 前回F2/F3記録commit：`e486e27cbabdec4387f9d183a758720e1cf2caee`。
- lint是正・wheel source：`f6d28dde184ff1950c4729197a3a9ac1645b0c11`。
- 統合予定の記録付き候補：`review/f3-lint-correction-20260906`。以降はF4文書・証跡追加のみ。
  最終headはPRと完了応答で特定する。本体修正承認後は新候補として再固定する。
- F1 source digest（testsも含む、是正前）：
  `05e3c3b52b2e42e1156eeb5facde98c2ba4215214b61b62d0606705a99d11e06`。
  是正後：`13d7c16e717db7e9a5f60c3e39c50746fb443c290d44d09e353cc478a5ce465e`。
  差は5テストのimportのみ。本体・build・依存関係・計測fixture不変を新manifestへ記録した。

## 1. 統合前（別承認が必要）

1. 完了済みlint是正をやり直さず、[F4で検出した3種の互換性failure](f4_premerge_review_20260906.md)の
   最小修正・再検証範囲の承認を受ける。Clang constexpr、Python 3.8 driver、Windows slot tests。
   CI設定・閾値・対象範囲を緩めない。修正後もmainへのmergeは別の明示承認が必要。
2. 読取りで最新mainと候補を再確認する。

   ```bash
   git status --short --branch
   git ls-remote origin refs/heads/main
   git rev-parse review/f3-lint-correction-20260906
   git diff b202e6a0cbb2eded9bc2ee5e59f750428e73ca49 -- src csplendor CMakeLists.txt setup.py pyproject.toml
   ```

3. mainが進んでいれば新しいmain SHAをisolated worktreeで比較する。既存mainをcheckout/reset/stashしない。
   衝突・本体/build差分があれば統合後treeを新候補として固定し、変更範囲の回帰、reference/VERIFY、
   sanitizer、該当F1 primary/guardを再実行する。旧F1の全数値を新treeの実測と称さない。
   文書だけの進行なら実装/build不変をhashで確認して証跡を再利用できる。
4. 作業branch通常push・PR #26作成は承認済みで実施した。以後は既存PRを使用し、重複作成しない。
   main/masterへ直接pushしない。保護設定と実checkを再確認し、明示承認後だけPR経由で統合する。
   mainへの統合前に再度base/head完全SHAを確認する。force push、未承認branchの一括取り込みはしない。

### CIと追加gate

現在の `.github/workflows/ci.yml` が定義するjob（実行済みとはしない）：

- `test`：Python3.8–3.12、Python compile/lint/security lint、pytest警告error、3.12 coverage50%。
- `package`：wheel/sdist、twine/auditwheel診断、sdistから再build、隔離wheel smoke。
- `native`、`cpp-coverage`（C++ coverageはreport-only）。
- `cross-platform`：macOS arm64 portable/native、Linux arm64 portable、Windows x64 portable。
- `strict-bindings`：GCC/Clang warning-as-error。
- `sanitizer`：TSan、ASan/UBSan。
- `nightly-native-soak`：schedule/manualのみ。PRで自動実行したと扱わない。

F4前半でbranch APIはprotected=false、強制required checksなし、適用rulesも空を確認。
強制されなくてもworkflow全体をgateとする。初回hosted CIは9 SUCCESS・7 FAILURE・適用外skip1。
最終PR headの結果はPR本文と前半報告から確認する。F1のローカルnative44等は再利用するが、
Clang sanitizer build失敗を実行PASSへ読み替えない。利用側dirty treeの配備承認も含めない。

## 2. merge後のclean build/install（今回は未実行）

merge後の完全SHAを取得し、そのSHAから新しいdetached worktreeと新しいvenvを作る。
以下の`<...>`は承認後に決める一意な値であり、そのまま実行しない。既存dirを再利用・削除しない。

```bash
git worktree add --detach /home/kuboyu/workspace/repos/csplendor-release-<id> <merged-full-sha>
python -m venv /home/kuboyu/workspace/repos/csplendor-release-env-<id>
```

専用shellを新規worktreeへ移し、新venvのPythonのみを使う。常用Pythonのpipは使わない。
F1に合わせるならPython3.12.1、GCC15.2、pybind11 3.0.1、NumPy2.2.6を選び、
setuptools/CMakeを含む実際のbuild依存versionを記録する。private indexの認証値や環境全体は保存しない。

```bash
cmake -S . -B build/release-native \
  -DCMAKE_BUILD_TYPE=Release -DCSPLENDOR_CPU_TARGET=portable \
  -DCSPLENDOR_ENABLE_LTO=OFF -DCSPLENDOR_PERF_INSTRUMENTATION=OFF \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF -DCSPLENDOR_BUILD_NATIVE_TESTS=ON
cmake --build build/release-native --parallel 4
ctest --test-dir build/release-native --output-on-failure
/home/kuboyu/workspace/repos/csplendor-release-env-<id>/bin/python -m pip wheel \
  . --no-deps --wheel-dir build/release-wheels
```

wheelを1件だけと確認し、絶対pathで新venvへinstallする（旧環境へforce-reinstallしない）。
Python拡張のpybind11 LTOとnative追加LTO OFFは別設定。wheelをCPU portableで生成すること、
新しいbuild cacheのcompiler/flags、wheel SHA、展開された.so SHAを保存する。
新venvへ利用側の必要依存を承認済みpinで導入し、学習やmodel downloadはしない。

source外の新しい空cwd、汚染のないPYTHONPATHでimportし、`csplendor.__file__`、
`csplendor._csplendor.__file__`、`.so` SHA、version、compiler flagsを記録する。
`Game(seed=42)`、合法手数、snapshot、owning NumPy、実モデル2ply、mate/session/frontierの
最小受入を**実install先**で行う。F2 probeはF1拡張のpath/SHAにfail-closedなので、
新wheelへ無条件転用せず、受入側のexpected identityを新build証跡に基づき別記録として用意する。

## 3. 実利用側の更新対象（要承認）

| 対象 | 確認できた接続 | 更新・確認するもの |
|---|---|---|
| dlsplendor | Python packageがcsplendorをimport。requirements/setupにはcsplendorのversion pinなし | 承認した実行venv内のcsplendor wheel/pin。利用側source/config/modelは別承認なしに変えない |
| GUI AIワーカー | `DLSPLENDOR_PYTHON`、`DLSPLENDOR_ROOT`、継承PYTHONPATH。V3 Python MCTS | Pythonを新venvへ向ける。古いeditable/.soが先にimportされないことをprocessで確認 |
| GUI mateワーカー | `CSPLENDOR_PYTHON`、`CSPLENDOR_ROOT`、継承PYTHONPATH | rootが古いcsplendorを先頭へ追加し得るため、Python変更だけで終えず両方の解決先を照合 |
| checkpoint/config | selfplay17/best.pt＋selfplay16_exact_mate.yaml（推奨）、selfplay12比較経路 | 同一model/config SHAを維持。学習、重み変換、探索予算の暗黙変更をしない |

常用のlaunch設定ファイル・実行Python・deployment service/pinは未確定。
`.env`や秘密情報を読み取って推測せず、F4承認時にユーザーから更新対象の指定を受ける。
環境変数は承認した起動processにだけ適用し、外部repoのtracked設定を書き換えない。
worker再起動は保持session/TTを失うため、利用者の処理を中断しない時点で行う。

## 4. 旧版への復帰

1. 更新前に旧venv/wheel、実import path/SHA、旧dependency pin、起動時の**非秘密**の設定だけを記録する。
   `.env`内容の複製や環境全量dumpはしない。旧成果物、旧model/config、局面snapshotを保持する。
2. 問題が出たら新workerを停止し、`DLSPLENDOR_PYTHON`/`CSPLENDOR_PYTHON`とroot/PYTHONPATHの
   選択を旧環境へ戻してworkerを再起動する。稼働中session/TTを旧binaryへ流用しない。
3. 旧成果物がない場合、既知のmain `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` から
   **別dir・別venv**へbuild/installする。このSHAはF1比較の既知mainであって、未確認の常用旧版SHAではない。
   旧.soのpath/SHAを確認し、保存局面の読込み→合法手→小さな実モデル/worker smokeを行う。
4. Git側の取り消しまで必要なら、別作業branchで通常のrevert PRを提案し、改めて承認を受ける。
   reset、履歴改変、force push、mainへの直接pushを復帰手段にしない。

tag、GitHub Release、PyPI公開、branch/worktree/旧wheel削除はF4のmerge承認とも別範囲。
これらは一切実行していない。F4前半の実施と区別し、**merge・導入は承認待ち**。
