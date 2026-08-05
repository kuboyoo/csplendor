# R8 テスト・文書・残存debt整理

実施日: 2026-08-05
状態: 完了

## テストの責務名

リファクタリング履歴を表す`phaseN`名を、失敗時に対象が分かる責務名へ変更した。

| 旧名 | 現在名 |
|---|---|
| `test_phase0_contracts.py` | `test_engine_baseline_contracts.py` |
| `test_phase0_benchmark_runner.py` | `test_benchmark_contracts.py` |
| `test_phase2_move_generator.py` | `test_rule_move_generation.py` |
| `test_phase3_mcts_searcher.py` | `test_mcts_searcher.py` |
| `test_phase4_encoder_common.py` | `test_encoding_common.py` |
| `test_phase5_bindings.py` | `test_binding_callbacks.py` |
| `test_phase6_dfpn_modules.py` | `test_solver_dfpn_modules.py` |
| `test_phase6_mcts_contracts.py` | `test_mcts_contracts.py` |
| `test_phase6_rule_transitions.py` | `test_rule_transitions.py` |
| `test_phase7_storage_contracts.py` | `test_domain_storage_contracts.py` |
| `test_phase7_undo_record.py` | `test_domain_undo_record.py` |
| `test_cross_phase_review.py` | `test_cross_subsystem_contracts.py` |

`test_test_taxonomy.py`で履歴名の再導入を検出する。過去のbenchmark script
`benchmark_phase0.py`は再現用artifactのpathなので改名せず、現在のtest名から履歴を外した。

## Native testの分割

単一だったMCTS scheduler executableを、共有suite objectと次の実行単位へ分けた。

- `mcts_scheduler_lifecycle`: queue、session再利用、reset、設定、root noise。
- `mcts_scheduler_limits`: timeout、cancel、capacity、overflow、callback/worker failure。
- `mcts_scheduler_hidden`: backend/thread別hidden-information determinization。

各mainは`tests/support/native_test.h`の共通runnerを使い、例外にsuite名を付ける。CMake labelで
subsystem、stress、replayを選択でき、nightlyは`stress|replay` labelだけを25回反復する。

## Shared differential support

`tests/support.py`へ再現可能なreachable/random corpus用の`assert_differential_corpus()`を追加した。
新しいrule property testはmaterialized `legal_actions`のpacked列とnative
`legal_action_codes`を、固定seedの共有局面列で全手・全順序比較する。

## Public headerとC++ coverage

互換性manifestで`public`に分類した16 headerをCMakeで1本ずつ単独include、compile、linkする。
Python testがCMakeの一覧とmanifestの差を検出するため、公開追加時のmatrix更新漏れも失敗する。

GitHub ActionsへGCC/gcovrのC++ coverage jobを追加した。閾値は設定せず、text/XML artifactを
14日保持する。Ubuntu x64で全31 native testを実行した初回値は次のとおりだった。

| 指標 | 初回値 |
|---|---:|
| line | 71.6% (6,750 / 9,432) |
| function | 82.6% (694 / 840) |
| branch | 48.3% (4,473 / 9,259) |

GCCの既知の負hit出力はfileごとにwarningとして記録する。solver実装などnative testだけでは
偏りが大きいため、この値を直ちにgate化せず、複数revisionの推移を確認してから閾値を決める。

## 文書と残存不整合

`architecture.md`、`overview.md`、`engine_specs.md`、`tasks.md`を現行レイヤー、色順序、
貴族選択、MCTS/parallel statusへ合わせた。監査で見つかった`Board.__repr__`の旧色ラベルも
`D,S,E,R,O,G`へ修正し、native/Python/API間のID対応を回帰testで固定した。

第1次計画は完了した履歴として固定し、第2次計画R0--R8の詳細記録をindexから辿れるようにした。

## ローカル検証

| 検証 | 結果 |
|---|---:|
| editable build | 成功 |
| Python通常test（warningをerror化） | 481 passed, 4 deselected |
| Python性能test | 4 passed |
| Python coverage | 54.04%（50% gate通過） |
| native Release CTest | 31/31 passed |
| public header standalone | 16/16 passed |
| C++ coverage生成 | text/XML成功 |
| native stress/replay反復 | 4 test×25回、100/100成功 |
| Clang TSan | 31/31 passed |
| Clang ASan/UBSan（LeakSanitizer有効） | 31/31 passed |
| GCC/Clang strict binding build | 成功 |
| sdist/wheel、twine、auditwheel、隔離smoke | 成功 |

全OS/architectureとPython 3.8--3.12は同revisionのPull Request CIを最終gateとする。
