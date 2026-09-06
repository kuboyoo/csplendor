# F3是正：追加テスト5ファイルのCI lint（2026-09-06）

## 判断：READY_FOR_REVIEW

承認された7件をimport周辺だけの最小修正で解消した。CIと同じlint対象・rule、
関連73テスト、Python 3.12の警告error＋coverage gateが通過した。
**リモートCI未実行。出荷承認ではない。F4は未実行・承認待ち。**
main変更、merge、push、PR作成、常用install/設定変更、公開はしていない。

[前回のBLOCKED報告](f2_f3_shipping_review_20260906.md)と旧raw/manifestは履歴として保持する。
今回の正本は本書と[是正manifest](f3_lint_manifest_20260906.json)、
新規 `raw/f3_lint_20260906/*.json.gz` 16件。

## 候補・証跡のidentity

| 対象 | 完全SHA / SHA256 |
|---|---|
| F1計測・F2受入の本番コード | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| F1記録commit | `49878b661298bf45e39c5f5ca5afa6d0e363736a` |
| 今回の開始HEAD／前回F2・F3記録commit | `e486e27cbabdec4387f9d183a758720e1cf2caee` |
| F1～前回F2・F3のsource digest | `05e3c3b52b2e42e1156eeb5facde98c2ba4215214b61b62d0606705a99d11e06` |
| 是正後のsource digest（testsを含む） | `13d7c16e717db7e9a5f60c3e39c50746fb443c290d44d09e353cc478a5ce465e` |
| 使用したPython拡張SHA256（F1/F2と同一） | `f2f653f4794d5454be82bce17ee4cb0ac38f9141fc7789e4275a7d01f01bb597` |

作業cwdは `/home/kuboyu/workspace/repos/csplendor-final-candidate`。
開始時clean、指定SHAと一致。そこから `review/f3-lint-correction-20260906` を作成した。
ユーザー変更の取り込み・破棄、reset/stashはない。元のcsplendor worktreeと
ローカルmain/origin/main追跡ref（`7835f642b23251d0cb91de180006084521c74aa6`）の不変性を照合。
リモートmainは今回は再照会せず、F2/F3観測の
`f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` を履歴として参照する。F4実行時に再確認が必要。

source digestはF1と同じ選択・計算法（CMakeLists.txtとsrc/csplendor/scripts/tests内の
対象拡張子。各SHA256＋2 spaces＋相対path＋LFをpath順に連結してSHA256）。
全source一覧は `final_validation.json.gz`。前後の対象ファイル集合は同一で、
hashが変化したのは下記5テストだけ。関数・クラス・decoratorのASTはすべて同一。
本番実装、build入口、依存定義、CI/lint設定、計測コード・fixtureはF1から変更なし。

記録追加後のcommitは「5テストのimport是正＋今回の文書・検証補助・raw」であり、
F1の計測コードcommitとは区別する。循環する自己SHAは埋め込まず、完了応答に完全SHAを記載する。
ローカル記録commitの特定：

```bash
git log -1 --format=%H -- doc/performance_experiments/f3_lint_correction_20260906.md
git rev-parse review/f3-lint-correction-20260906
```

## 違反と最小修正

Ruff **0.13.3**。配置済みPythonのRuffを使用（隔離venv側のRuff実行ファイル不足は
前回記録済み。今回は再installや依存変更をしていない）。CIの設定・対象をそのまま実行：

```bash
python -m ruff check --target-version py38 --select E4,E7,E9,F,W,I csplendor tests
python -m ruff check --target-version py38 --select S csplendor
```

| ファイル | 修正前の行:列・rule | 実施内容 |
|---|---|---|
| `tests/test_build_profiles.py` | 2:1 I001、10:1 E402、11:1 E402 | import整形。既存sys.path設定の後、同じ2モジュールを`importlib.import_module`で取得 |
| `tests/test_compact_phase0_evidence.py` | 3:1 I001 | import直後の余分な空行1行を削除 |
| `tests/test_return_rank_selection.py` | 2:1 I001 | docstring/importとthird-party/local importの空行整形 |
| `tests/test_state_feature_numpy.py` | 1:1 I001 | third-party/local import間の空行追加 |
| `tests/test_v3_payment_codec.py` | 2:1 I001 | docstring後の空行、alias付きfrom importを同一moduleの2行へ分離 |

修正前に7件を再現し `lint_before.json.gz` に保存した。修正後の全対象lintはexit 0。
assert、fixture、parameterization、skip、収集対象は変更していない。
`noqa`などの抑制指定、設定緩和、除外拡大はない。
通常importと同じmodule名・sys.modules cacheを利用し、`scripts/benchmark_manifest.py` と
`scripts/run_paired_benchmarks.py` の実pathおよびaliasのobject identityを別processでも照合した。

## 再検証結果とCI範囲

| 検査 | 結果 | 証跡 |
|---|---|---|
| CI lint：E4,E7,E9,F,W,I／csplendor＋tests全体 | PASS、0件 | `lint_after.json.gz` |
| CI security lint：S／csplendor全体 | PASS | `security_lint.json.gz` |
| CI compileall：csplendor | PASS | `compile.json.gz` |
| 修正5ファイル＋関連4ファイル | **73 passed**、警告error | `targeted_tests.json.gz` |
| 同9ファイルの修正前後の収集 | **73 ID・順序一致** | `collection_before/after.json.gz`、`final_validation.json.gz` |
| テスト定義／decorator／assert等のAST | 5ファイルすべて不変 | `test_semantics.json.gz`、`final_validation.json.gz` |
| CI Python 3.12 pytest＋coverage gate | **595 passed, 1 skipped, 4 deselected**。coverage **58.89% ≥ 50%** | `ci_python_coverage.json.gz` |
| skip理由確認 | `generated/mate_puzzles2` 不在による既存条件。新規skipではない | `skip_reason.json.gz` |
| helper構文・import identity | PASS | `helper_compile.json.gz`、`import_identity.json.gz` |
| 文書リンク・raw hash・最終helper構文・source不変性 | PASS | `documentation_validation.json.gz` |

関連4ファイルは `test_engine_benchmark_tools.py`、`test_ml.py`、`test_encoders.py`、
`test_encoding_schema.py`。変更26件＋関連47件。performance 4件の除外は
CIが使う既存 `pyproject.toml` の `-m "not performance"` による。設定を変更していない。

Pythonテストは既存隔離venv `build/f2-env/bin/python`（3.12.1）を使用した。
実ロード拡張は候補内の
`csplendor/_csplendor.cpython-312-x86_64-linux-gnu.so`。F1のportable Release buildと同一SHA。
新native LTOはOFF、Python拡張の既存pybind11 LTOはそのまま。本体を再build/installしていない。
cache/coverageは候補の無視対象 `build/f3-lint-*` に隔離し、外部repoを検証import pathへ追加していない。

今回実行したPython 3.12コマンドはCI定義と同じ：

```bash
build/f2-env/bin/python -m compileall -q csplendor
build/f2-env/bin/python -m pytest -W error \
  --cov=csplendor --cov-report=term-missing --cov-fail-under=50
```

ただしCIのclean dependency install・hosted runner全体を再現したとはしない。
Python 3.8–3.11、package/wheel/sdist、cross-platform、strict-bindings、cpp-coverage、
TSan、nightly soak、remote required checks／branch protection確認は今回未実施。
native/reference/ASan等はF1証跡を再利用し、今回再実行していない。
CI job一覧と統合gateは[F4 runbook](f4_integration_runbook_20260906.md)を参照。

## F1・F2証跡の再利用と残る制約

F1 manifest・raw 100件＋CSV、F2/F3 manifest・raw 14件のhash不変を確認した。
旧失敗rawも含め上書きしていない。本番・build・計測fixture不変なので全paired A/B、
実モデル受入、全native監査を無条件に繰り返していない。F1倍率・信頼区間・RSSは変更なし。
並列MCTSの累積高速化、LTO追加効果は未確定。AI全体や7手詰め完遂時間へ外挿しない。

前回F2の実モデルCPU、実GUI handler/frontier、session再利用・中断、保存再読込み、
NumPy所有権の証跡を再利用。ブラウザ・HTTP全体、GPU、native48実NN、実モデルA/B、
長時間対局・棋力、常用導入は未実施のまま。Genbuは前回確認の必要file不足を維持し、
今回モデル探索や外部repoの再検証をしていない。外部dirty dlsplendorを配備pinとして承認したものではない。

追加テストのlintという停止原因は解消した。既知の重大な本体/build不整合は検出していない。
次は是正差分と証跡のレビュー、およびF4の明示承認。承認後だけ最新base/headを再固定し、
作業branch push→PR/required CI→review→統合、別worktree/venvのclean install受入へ進む。
常用環境の更新対象の指定、旧venv/wheel/起動先を保持する復帰手順はrunbookに残す。
**READY_FOR_REVIEWはremote CI通過・merge・出荷許可を意味しない。ここで停止する。**
