# F2・F3受入確認／F4実行準備（2026-09-06）

## 追記：承認済みF3是正を完了

追加5テストのlint違反7件をimport周辺だけで解消した。CI lint/security lint、関連73テスト、
Python 3.12の595テスト＋coverage 58.89%が通過。現在の判定は **READY_FOR_REVIEW**。
詳細・新source digest・記録commitの特定方法は[是正報告](f3_lint_correction_20260906.md)と
[新manifest](f3_lint_manifest_20260906.json)を正とする。リモートCI未実行、**F4は未実行・承認待ち**。

以下は `e486e27cbabdec4387f9d183a758720e1cf2caee` 時点の原報告を保持した履歴である。
「今回」「未修正」「BLOCKED」および旧source digestはその時点を指し、是正後の状態ではない。
原報告の自己commit照会は、更新後には本書を変更した是正commitを返すため、原版は上記完全SHAで特定する。

## 判断：BLOCKED（CI lint、F4は未実行・承認待ち）

実モデルの限定受入とGUIワーカー連携は通過した。一方、現行CIと同じlintコマンドが
候補追加テスト5ファイルで7件失敗し、同じmain側は通過した。F1の「実行テスト通過」を
「PR必須チェック通過」へ読み替えない。候補を固定するため既存テストを勝手に修正せず、
修正案・再検証範囲を記録して停止する。本体/buildの修正が必要な実行不整合は検出していない。

F0/F1全監査・全benchmarkは再実行していない。F1 raw 100件・CSVのSHA、既存検証結果、
拡張binaryのSHAを照合して再利用。新たなrawは `raw/f2_f3_20260906/` に追記した。
今回の差分はREADME日英・変更履歴・報告・検証補助・証跡のみ。
本体、build設定、既存テスト、reference/VERIFY/fallbackは変更していない。

## Identityと対象範囲

| 対象 | 完全SHA / digest |
|---|---|
| 検証済みエンジンcommit | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| F1記録commit／今回の開始HEAD | `49878b661298bf45e39c5f5ca5afa6d0e363736a` |
| リモートoriginのmain（開始・終了照合） | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` |
| ローカルmain／origin/main追跡ref（更新していない） | `7835f642b23251d0cb91de180006084521c74aa6` |
| candidate source digest（F1と同じ） | `05e3c3b52b2e42e1156eeb5facde98c2ba4215214b61b62d0606705a99d11e06` |
| ロードしたPython拡張SHA256 | `f2f653f4794d5454be82bce17ee4cb0ac38f9141fc7789e4275a7d01f01bb597` |

作業対象は `/home/kuboyu/workspace/repos/csplendor-final-candidate`、新しい隔離branchは
`review/f2-f3-shipping-20260906`。開始時clean。元のintegration branchやユーザーworktreeは変更しない。
source digestの選択規則はF1と同じで、`setup.py`等のbuild入口も別途hash・差分を照合した。
`b202e6a`→`49878b6`→今回の作業treeで実装/build/既存テスト差分はない。

記録追加後のcommitは**報告・検証補助のcommit**であり、別のengineを再測定したものではない。
自分自身のcommit SHAをファイル内へ埋め込む循環を避け、完了応答に完全SHAを記載する。
ローカル記録を特定するコマンドは
`git log -1 --format=%H -- doc/performance_experiments/f2_f3_shipping_review_20260906.md`。
F4ではその記録を含むbranch tipを完全SHAへ固定し直し、本書のcode digestと照合する。
現時点ではlint未解消なので、このSHAを無条件にmainへ統合してはならない。

## F2：実利用の確認

利用側は名称から推測せず、AGENTS、README、GUIモデル登録・起動コード、config、実ファイルを確認した。

- dlsplendor：`/home/kuboyu/workspace/repos/dlsplendor`、HEAD
  `a46adb2dd5882782d0df19f7d545bc2ef4486ccb`。**未コミット変更あり**。
  実利用の観測として現在のsourceを読み取り専用で使用し、source hash一覧を開始・終了で照合した。
  このdirty treeを上記HEADだけで再現可能とはしない。変更はcsplendorへコピー・merge・commitしていない。
- splendorgui：`/home/kuboyu/workspace/repos/splendorgui`、HEAD
  `eb023f23a8c87b30bb0d2bb4373a7efac9cd6ea8`、cleanを保持。
  READMEはselfplay12を既定と記すが、実際の`src/lib/server/dlsplendorModels.ts`は
  selfplay17をrecommendedとしている。両方を検証し、外部READMEは編集しない。

新venv `build/f2-env`（Python 3.12.1、system packages参照）と、F1の隔離candidate buildを使用。
`PYTHONPATH`で候補を明示し、bytecode/cacheの外部書込みを抑止した。pipの常用editable installは上書きしない。
実ロード先は
`/home/kuboyu/workspace/repos/csplendor-final-candidate/csplendor/_csplendor.cpython-312-x86_64-linux-gnu.so`。
子frontier workerも `/proc/<pid>/maps` から同じ拡張path/SHAを確認した。

build条件はF1の実compile/link証跡を再利用：GCC15.2、portable Release、
`-O3 -DNDEBUG -std=c++17 -fPIC -fvisibility=hidden -flto=auto -fno-fat-lto-objects`、
linkにも既存pybind11 LTO。新たなnative LTO opt-inはOFF、PERF/VERIFYなし、fast-mathなし。
wheelの新規build/install受入ではなく、**同一SHAのin-place拡張を隔離venvから使用した受入**である。

### 実モデル・実効探索条件

| model（外部repo内、コピーしない） | SHA256 | config |
|---|---|---|
| `models/selfplay12/best.pt` | `06542a55a1548d722d379a0576ffd64f304eacca6255aaf8aa6d4b17661945ec` | `configs/selfplay12.yaml` |
| `models/selfplay17/best.pt` | `878a8d4eb0b1f01c1de1ba1dfeb23abb33646cdc7dd6e487dc2bde0157da4ecd` | `configs/selfplay16_exact_mate.yaml` |

PyTorch2.10.0+cu128だが実行deviceは明示的に**CPU**、float32、Torch intra-op2 / inter-op1。
backendは`dlsplendor.search.mcts.MCTS`（Python PUCT）で、native legacy/shared/rootではない。
V3/3133 action、`simple_payment_mode=False`（全支払い）、canonical196＋公開統計117＝313特徴。
この利用側は`StateEncoder.encode_canonical`＋`encode_public_card_statistics`を使用し、
5C-Bの`StateFeaturizer`置換経路ではない。native 48手の実NN受入をしたとは主張しない。

seed42、2ply、各400 simulations、noiseなし、GUI同様playout-cap randomizationだけOFF。
determinization/選択的chance/tree reuseはON、leaf batch128、通常MCTSの時間上限は0。
戦術予約・戦略候補の追加予算600は設定を保持し、実消化simulation数はrawで区別する。
selfplay17のmate gateは9点、depth1–3、20,000 nodes、20ms、jobs1、cache50,000。
全実効SearchConfig、モデル/configのhashはrawに記録している。

### 実施結果

| 項目 | 結果・範囲 |
|---|---|
| 実モデルload→snapshot読込み→特徴量→NN評価→MCTS→合法手→遷移 | 両モデルPASS、各2ply×400 sim。出力finite、方策shape/正規化、合法性を検査 |
| 実GUIセッションhandler | 両モデルPASS、各2手×16 sim、作成・着手・削除。HTTP/画面の試験ではない |
| dlsplendorの実MateSearchSession接続 | 両モデルPASS。既存の人工1手詰めで証明手をV3へ変換、MCTSを上書き。warmは1 node/1 reused hit |
| mate設定の違い | selfplay12は検証インスタンス内だけ有効化し1秒、selfplay17は配備設定の20msで通過。NNは実モデル、solverは実実装 |
| owning NumPy／torch.from_numpy | float32・C連続・owning・base=None、196/313 shape、探索/局面変更/破棄後の保持値を照合PASS |
| GUI JSON-lines frontier | 実workerで5手・7手fixtureの選択経路を終局まで遅延展開PASS。訪問した守備ノードは全合法応手集合と一致、同じ要求のcache応答も一致 |
| 中断・再開・次手番session | 隔離venvで既存3テストPASS：外部事前cancel、node上限中断→再開、相手応手後のmemo再利用 |
| 保存・再読込み | rawへ保存したsnapshot/child_stateを別processで再読込みし、byte/feature/着手遷移・勝者を照合。永続TT保存の新機能はない |
| GUI既存bridge tests | 7 PASS。うちcache上限/引数転送には既存mockもあり、実モデル/実frontier通過と混同しない |

単発の候補実測ではselfplay12の400-sim処理が150.79/137.43ms、selfplay17が153.59/147.91ms。
model loadは29.25/42.89ms、probe内全体は395.58/437.97ms（interpreter起動・import除外）。
計時中のaffinity/負荷をF1同様には制御しておらず、単発の受入観測値で**CIなし、main比較なし**。
モデル間の速度比較やAI全体の高速化率には使わない。F1倍率は変更しない。

### 未実施・失敗の区別

- **未実施**：ブラウザ描画・HTTP/Next.js経由の全体フロー・画面中断、GPU/CUDA/MPS、
  native48実NN、実モデルpaired A/B、長時間対局/棋力、全checkpoint、常用環境への導入。
  GUI自体は存在するが、外部repoの`.next`/設定を書き換える起動は行わず、F3の停止条件を優先した。
- **環境不足で未実施**：Genbu adapterの実参照先
  `/home/kuboyu/workspace/repos/alphazero-general-ori/splendor/SplendorGame.py` と
  `/home/kuboyu/workspace/repos/alphazero-general-ori/HeianKyo/genbu.pt` が存在しない。
  名前が似た別modelで代用せず、GenbuをPASSにしない。
- **修正済み検証補助の失敗**：初回probeが存在しない`Game.apply_action`を呼んだ。
  既存GUIの`Game.apply`/V3 action ID観測に合わせ、snapshotをJSON用hexへ変換して再実行PASS。
  エンジンの修正ではなく、新規補助のみ。失敗rawを保持。
- **環境toolの失敗**：system packages参照venvで`python -m ruff`のbinaryが見つからなかった。
  常用installを変更せず、配置済みPython/Ruff0.13.3で同じread-only検査を実行。
- **候補固有の未解決失敗**：下記CI lint 7件。実行solver/NNのfailureと区別する。

## F3：mainとの差分レビュー

全ファイル分類は[CSV](f3_diff_inventory_20260906.csv)、集計・hash・証跡索引は
[manifest](f2_f3_manifest_20260906.json)。既存採否一覧は
[Phase 6共通監査](phase6_common_audit_20260906.md)とF1統合報告を再利用した。

- 採用コード：exact hash、noble mask、合法手count/table/packed sink、solver path/class/reason、
  3B sidecar、3C full-key unordered TT、score-once、live-invocation scratch、visible通常rollback、
  take代表化、返却順位選択、legacy管理表統合、owning NumPy、既定OFFのLTO option。
- テスト：上記の独立oracle、順序/上限/rollback/hidden/schema/binding/build-profile回帰。
  棄却V3 DPや特徴量表のoracleが残ることを「本番採用」と解釈しない。
- 計測補助：paired/manifest/差分harness、PERF counters、並列queue/lock/ledger診断、圧縮raw。
  shared並列の変更は主にPERF専用診断であり、棄却4C案の採用ではない。
- 文書：採否・API/契約、過去raw索引、README日英、今回の変更履歴・再現/統合/PR案。
- 無関係な変更：未分類のものは検出していない。mainの情報集合v2/frontier修正は既にmain側にあり、
  今回のmain差分を別機能branchの無条件取り込みとはしない。外部dirty treeは統合対象外。

重点照合：ヒントsort/rotate→truncateのcompact/reference接続、sidecarとBoard同一scope復元、
TT solve前後trim（hard RSS capではない）、scratchの非alias、visible/reveal rollbackの採否、
NumPy所有権/GILを確認。reference/VERIFY/fallbackの削除、棄却案の復活、数値演算順変更はない。
本体はF1と同じため、F1 native44/Python595/reference/ASan証跡をそのまま参照する。

### 統合前に解消が必要：lint 7件

`.github/workflows/ci.yml`の
`python -m ruff check --target-version py38 --select E4,E7,E9,F,W,I csplendor tests`
に相当する検査（`--no-cache`のみ追加）。main PASS、候補FAIL。

| ファイル | 違反 | 最小修正案（未実施） |
|---|---|---|
| `tests/test_build_profiles.py` | I001×1、E402×2 | import整形。既存の動的path設定を維持し、他harness同様の明示loaderまたは理由付き局所E402注記 |
| `tests/test_compact_phase0_evidence.py` | I001×1 | import blockの余分な区切りを整える |
| `tests/test_return_rank_selection.py` | I001×1 | third-party/local importを区切る |
| `tests/test_state_feature_numpy.py` | I001×1 | import順を整える |
| `tests/test_v3_payment_codec.py` | I001×1 | aliasを含むfrom importを整える |

修正承認後は5ファイルに限定し、CI lintとこれら5ファイルのpytest、source/build差分照合を実施する。
assert削除・CI rule緩和・既存raw上書きはしない。import/注記だけなら本体binary再buildやF1速度A/B全体は不要。
もし本体/buildまで変える必要が出た場合は別候補として固定し、影響する回帰/VERIFY/sanitizerと
primary/guardを再検証してから承認を求める。

並列MCTS累積高速化・統合後LTO追加効果の未確定、legacy未展開node主体でRSS約18.6%増、
soft deadline、他OS/Python3.8実機未検証を保持。CI/branch protectionのリモート実状態は未確認で、
実PR作成・CI起動をしていない。設定上のjob一覧は[F4準備](f4_integration_runbook_20260906.md)へ記載した。

## 成果物と停止

[再現手順](f2_f3_reproduction_20260906.md)、[PR本文案](f3_pr_draft_20260906.md)、
[F4統合・復帰手順](f4_integration_runbook_20260906.md)を保存した。
main／ユーザー変更／常用install／外部repo設定は保持。push、merge、実PR、tag、Release、PyPI、
branch/worktree削除は未実行。**F4は未実行・承認待ち**。先にlint修正範囲の承認と再検証が必要。
