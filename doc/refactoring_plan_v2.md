# csplendor 第2次リファクタリング計画

## 1. この文書の位置付け

この文書は、2026-08-05 時点の `csplendor` 全体を俯瞰し、次のリファクタリングで守る契約、目標構成、実施順序、完了条件を整理した計画である。

調査基準はローカル追跡情報上の `origin/main`（`b13ebe4`）であり、調査開始時の作業ツリーは同コミットと内容上の差分がない。本書は計画とその進捗を扱い、完了した先行項目は本文へ反映する。

既存の [`doc/refactoring_plan/README.md`](refactoring_plan/README.md) は、第1次リファクタリングの実施記録として保持する。本書は、その完了後に追加された MCTS、並列探索、ソルバー、Web API を含む現在の構成を対象とする第2次計画である。

## 2. 目的

第2次リファクタリングの目的は、現在の速度と互換性を維持しながら、次の変更を安全に行える構造へ移行することである。

- ルール、状態更新、エンコード、探索の責務と依存方向を明確にする。
- 公開 API と内部実装を分離し、内部表現を段階的に改善できるようにする。
- 巨大なヘッダー、binding、探索実装、Python モジュールを責務単位に分割する。
- C++ と Python に重複する仕様・変換ロジックの正本を明確にする。
- editor 状態、通常対局状態、探索内部状態で異なる不変条件を明文化する。
- Linux、macOS、Windows、x86_64、arm64 の既存ビルド・テスト経路を維持する。
- リファクタリングの各段階で、全手同値性、決定性、性能を機械的に確認できるようにする。

次の項目は本計画の非目標とする。

- ゲームルール、合法手集合、合法手順序の変更
- action ID、feature layout、snapshot、USI、trace の仕様変更
- MCTS やソルバーの探索アルゴリズム変更
- 並列 MCTS の experimental/stable 区分の変更
- GUI、学習処理、モデル、棋譜データの本リポジトリへの追加
- ファイル分割だけを目的とした全面的な書き換え

仕様変更が必要と判明した場合は、リファクタリングと同じ変更に混ぜず、移行方針と互換性を決める別タスクとして扱う。

## 3. 現状の俯瞰

### 3.1 規模と責務の集中

追跡対象ファイルのおおよその行数は次の通りである。

| 領域 | 行数 | 主な責務 |
|---|---:|---|
| `src/` | 16,607 | C++ ルール、状態、エンコーダー、MCTS、ソルバー、binding |
| `csplendor/` | 3,910 | Python 公開 API、feature/action 補助、Web API |
| `tests/` | 13,682 | Python/C++ の互換、探索、並行性、性能テスト |
| `scripts/` | 8,596 | DFPN、詰み問題生成、ベンチマーク、検証スクリプト |
| `doc/` | 10,661 | 仕様、実装計画、検証記録 |

特に責務が集中しているファイルは以下である。

| ファイル | 行数 | 集中している責務 |
|---|---:|---|
| `scripts/dfpn_mate_solver.py` | 3,475 | 探索、置換表、証明、入出力連携 |
| `src/bindings.cpp` | 1,829 | ほぼ全 C++ API の Python 公開 |
| `src/reveal_verified_solver.h` | 1,807 | hidden outcome、探索、証明 DAG、結果構築 |
| `src/mcts_parallel_searcher.h` | 1,663 | scheduler、worker、探索、失敗処理、結果集約 |
| `src/mcts_concurrent_tree.h` | 1,242 | 並列木、ノード、辺、同期、世代管理 |
| `csplendor/api/ai_manager.py` | 1,180 | 外部 AI 検出、モデル読み込み、探索方式選択 |
| `src/mcts_tree.h` | 1,039 | 逐次木、統計、展開、選択、互換処理 |
| `csplendor/api/usi_kifu.py` | 873 | USI 字句解析、合法手解決、局面更新、KIFU 変換 |
| `csplendor/api/app.py` | 770 | HTTP endpoint、session、棋譜、AI 呼び出し |

行数自体を問題とはしないが、独立して変更・検証したい責務が同じ翻訳単位やクラスに集まっている点を改善対象とする。

### 3.2 現在の依存構造

現在の主要な流れは概ね次の通りである。

```text
Python user / FastAPI
        |
        +--> csplendor Python helpers
        |       +--> Python ActionEncoder / StateFeaturizer
        |       +--> USI / replay / AI integration
        |
        +--> _csplendor (単一の bindings.cpp)
                    |
                    +--> Board / Player / Game / move generation
                    +--> action/state encoders
                    +--> serial / parallel MCTS
                    +--> visible / reveal-verified solvers
```

C++ コアは CMake 上で `INTERFACE` ライブラリとして構成され、実装の大半がヘッダーに置かれている。Python extension は巨大な `bindings.cpp` から一括してビルドされる。この構成はテンプレート最適化には有利だが、依存関係、ビルド時間、変更影響範囲、binding のレビュー単位を大きくしている。

### 3.3 維持すべき強み

現状には、次のような強い検証基盤と高速化資産がある。リファクタリングで失ってはならない。

- sink/emitter を用いた allocation-free の合法手生成
- compact edge、bitset、packed state を用いた MCTS hot-path 最適化
- versioned snapshot と backward compatibility test
- portable RNG、seed/state 管理、deterministic trace/replay
- Python/C++ encoder の同値テストと MCTS の旧実装比較
- strict warning、ASan/UBSan、TSan、package build の CI
- Linux、macOS arm64、Ubuntu arm64、Windows x64 の CI 構成
- pybind11 の lifetime、GIL、NumPy ownership に対するテスト
- editor API の失敗時 atomicity、hash、snapshot に対するテスト

## 4. 優先課題

### P0: 互換性契約と不変条件が暗黙的

`Board` と `PlayerState` は配列、packed 値、個数、履歴由来情報を同時に保持し、多くのフィールドが公開されている。通常のゲーム進行では成立する不変条件も、editor API で作られた局面では意図的に成立しない場合がある。

また、hash invalidation は呼び出し側の手作業に依存する。private 化を先に行うと公開 C++ API を壊し、単純な setter 化では hot path を悪化させる可能性がある。まず次の状態分類と契約が必要である。

- reachable state: 正規の初期局面から合法手だけで到達した状態
- editor state: 解析・テスト向けに一部の不変条件を緩和した状態
- search state: rollout/determinization/undo が内部的に一時利用する状態
- serialized state: snapshot/trace から復元可能であることを保証する状態

固定長コンテナの上限超過時の動作にも注意が必要である。現在の合法手 API は、異常な editor 状態で生成数が上限を超えた場合にも、先頭から一定数を保持する既存挙動を持つ。例外化や順序変更は純粋なリファクタリングではない。

### P0（解決済み）: gem 色 ID の層間不整合

調査中、gem 名と数値 ID の対応に既存の不整合を確認した。

- C++ `GemType`、カードデータ、sibling `usi/docs/USI.md` は `DIAMOND=0, SAPPHIRE=1, EMERALD=2, RUBY=3, ONYX=4` を正としている。
- 修正前の `csplendor/api/schemas.py`、`csplendor.GEM_NAMES`、`doc/engine_specs.md` の一部は `EMERALD=0, SAPPHIRE=1, RUBY=2, DIAMOND=3, ONYX=4` と解釈していた。
- 一部 C++ コメントにも後者の並びが残っていた。

2026-08-05 に `usi` リポジトリ、C++ コア、`dlsplendor` の feature/action 利用を横断確認し、C++/USI の `Diamond, Sapphire, Emerald, Ruby, Onyx, Gold` 順を正本と決定した。数値レイアウトは変更せず、Python schema・表示名・文書を正本へ合わせる。cross-layer golden test と state schema fingerprint により、既存モデルの意味を維持したまま将来の不整合を検出する。

### P0: コンパイル境界が弱い

コアの大半が header-only で、ほぼ全 binding が 1 翻訳単位にある。このため、次の問題がある。

- 小さな変更でも広範囲が再コンパイルされる。
- include の過不足や循環的な依存が見えにくい。
- private implementation を変更しても下流 C++ 利用者に再ビルドを要求する。
- binding の登録順序、型登録、GIL/lifetime の変更が巨大な差分に埋もれる。
- trace、snapshot、solver の非テンプレート処理まで各利用側でコンパイルされる。

### P1: ルール照会と状態更新の境界が重複

合法手生成、`Game::can_apply_*`、`Game::apply_*`、`rule_transition.h` の間で、予約元、購入費用、宝石返却、貴族選択、ターン終了条件に似た判断が存在する。合法手生成は高速化済みなので、共通化のために hot path を抽象化し過ぎないことが重要である。

一部の低レベル更新には、途中の検査が失敗した場合に部分更新を残し、solver 側の rollback を前提とする既存挙動がある。これを transactional に変えることも仕様変更であり、検証と移行なしには行わない。

### P1: encoder と feature schema の正本が分散

C++ には 48/V2/V3 action encoder と 196 要素の state encoder があり、Python にも独立した `ActionEncoder` と `StateFeaturizer` がある。主要経路は同値テストされているが、無効値への fallback などの境界挙動は完全には同一でない。

サイズ、offset、予約スロット、色順序が複数ファイルの定数として現れるため、将来の schema 追加時に片側だけ更新する危険がある。C++ を計算の正本にしつつ、Python の既存 fallback と import surface を保つ段階的移行が必要である。

### P1: MCTS に旧・新の複数 orchestration が同居

現在は、逐次 `MCTSSearcher`、prepare/apply batch、`ParallelMCTSSearcher`、root parallel の複数経路が存在する。`MCTS` は旧ノード所有、並列木、設定、RNG、世代、guard を広く所有する。

並列探索は実装が進んでいるが、実 NN 評価、品質 canary、seed variation、長時間 soak など stable 化の残ゲートがある。stable 化前に探索制御を大きく共通化すると、race、順序、trace の差分原因が分かりにくくなる。先に機械的なファイル分割と契約テストを行い、構造統合はゲート通過後に限定する。

### P1: solver に共通概念と巨大実装が混在

visible-only と reveal-verified solver は、state key、limit、action ordering、score/tie-break、terminal 判定などを個別に持つ。reveal-verified solver はさらに hidden outcome 列挙、oracle action、proof DAG 構築まで所有する。

Python 側の DFPN と詰み問題生成も、探索、出力、外部 engine adapter、永続化を大きな script 内で扱う。scripts は tests から module として import されているため、CLI を維持する薄い wrapper と内部 package を分ける必要がある。

### P1: Web API と外部 AI 連携が engine package に集中

`app.py` と `replay.py` は別々の process-global session を持ち、endpoint、状態保管、棋譜処理、AI 呼び出しが混在する。`usi_kifu.py` は構文解析から `Game` 更新、KIFU 出力まで担当する。

`ai_manager.py` は sibling repository の探索、`sys.path` 変更、複数モデル実装の検出・読み込みまで行う。csplendor の責務はルールエンジンと互換 API であるため、具体的なモデル discovery は将来的に `dlsplendor` 等へ移し、csplendor 側には provider protocol と互換 bridge のみを残すのが望ましい。ただし、これは複数 repository にまたがるため単独で削除しない。

### P2: テストと文書の構造が実装履歴を引きずっている

テストは広範で強い一方、phase 名のファイルや fixture/signature helper の重複が残る。C++ テストは少数の大きな executable に集中し、単独 header の自己完結性や subsystem ごとの失敗範囲が分かりにくい。

`architecture.md`、`overview.md`、`tasks.md` の一部は、現在の貴族選択、MCTS、並列探索、native encoder の実装状況を十分に反映していない。第1次計画の完了記録と現在の未完タスクも区別する必要がある。

## 5. 目標構成

最終的な依存方向を次のようにする。

```text
Python public API / compatibility wrappers
                    |
        responsibility-specific bindings
                    |
          compiled csplendor_core
             |              |
             |              +--> serialization / versioned schemas
             +--> domain --> rules --> encoding
                    ^          ^
                    |          |
             search adapters --+
                |          |
              MCTS       solvers

FastAPI endpoints --> application services --> session/kifu repositories
                                      |
                              public engine API

external AI provider protocol <--> dlsplendor / model repositories
```

移行は big-bang で行わない。公開 header、Python import、script entry point には forwarding wrapper を残し、旧実装との同値テストを保ったまま責務を一つずつ移す。

## 6. 変更中に固定する互換性契約

以下を変更しないことを各 PR の完了条件とする。

### Python/C++ 公開 API

- `csplendor.__all__` と既存 import path
- Python の関数・method 名、引数、default、返却型、例外型
- pybind11 object の lifetime と参照/コピーの意味
- NumPy array の shape、dtype、ownership、writable 属性
- 公開 C++ header の利用可否。変更が必要なら deprecation 期間を設ける

### ゲーム・エンコード

- 通常到達局面の合法手集合と生成順序
- editor 局面での上限時 prefix 保持を含む既存挙動
- `ActionType`、`GemType`、`Action::pack()` の数値契約
- 48/V2/V3 action ID と invalid/fallback の層別挙動
- 196 feature の shape と各 index の値
- snapshot version、USI/KIFU round-trip、replay 結果

### 探索・再現性

- seed/state の保存・復元
- position hash、observable hash、tree key
- deterministic scheduler の選択列と結果
- trace の byte 表現、digest、replay
- solver の action ordering、score/tie-break、proof DAG digest

### 性能

- hot path に新しい heap allocation、例外、virtual dispatch、不要な state copy を追加しない。
- 同一 binary、同一 seed、同一 fixture で旧新を交互に測定する。
- 中央値が旧実装比 0.97 未満、または RSS・build time が 5% 以上悪化した場合は原因を調査する。
- zero-latency evaluator の benchmark と、実 NN evaluator を含む benchmark を混同しない。

閾値内であっても統計的に意味のある悪化が見える場合は、理由を記録してから進める。

## 7. 実施フェーズ

### R0: 契約と基準値の固定

最初に、構造変更を評価するための基準を揃える。

実施項目:

1. Python/C++ API を `public`、`experimental`、`internal` に分類する。
2. （完了）gem 色 ID を C++、Python、JSON、USI、feature、モデル入力の各境界で監査し、golden fixture と schema fingerprint を追加する。
3. reachable/editor/search/serialized state の不変条件表を作成する。
4. 合法手上限、部分更新、hash invalidation、copy/reference semantics を明文化する。
5. include graph、clean build time、incremental build time、binary size、RSS の基準を保存する。
6. encoder、hash、snapshot、MCTS trace の golden digest を固定する。
7. 重複している test fixture、signature check、random reachable-state generator を共通 test support に集約する。

完了条件:

- gem 色は C++/USI 順を維持し、schema fingerprint の不一致を拒否する。
- 公開契約の一覧と golden test が CI で実行される。
- refactor 前 benchmark を同一環境で再現できる。

### R1: build と binding の分割

最初の製品コード変更は、挙動を変えにくく効果を確認しやすいコンパイル境界から始める。

実施項目:

1. `PYBIND11_MODULE` は 1 箇所に残し、登録を `bind_domain`、`bind_rules`、`bind_encoding`、`bind_mcts`、`bind_solvers` に分割する。
2. 型登録順序を明示し、相互依存する binding の順番を test する。
3. `csplendor_core` を段階的に実体のある static/object target へ移行する。
4. template である必要がない snapshot、trace serialization、formatting、solver の実装を `.cpp` へ移す。
5. public header ごとの standalone compile test を追加する。
6. portable/native CPU target、wheel、editable install の構成を維持する。

完了条件:

- `bindings.cpp` は module 初期化と登録呼び出しを中心とする薄いファイルになる。
- Python API surface、lifetime、GIL、NumPy ownership test が全て同値である。
- clean/incremental build time と wheel 内容を比較し、悪化がない。
- Linux/macOS/Windows の package CI が通る。

### R2: domain state と mutation gateway の整理

実施項目:

1. 固定容量 vector/stack の共通 primitive を作り、overflow policy を型または呼び出し側で明示する。
2. state profile ごとの `validate_invariants()` を追加し、debug/test で利用する。
3. canonical、derived cache、provenance/debug 情報をフィールド単位で分類する。
4. mutation と hash/cache invalidation を同じ gateway に集約する。
5. editor 用 mutation と hot-path の unchecked mutation を別 API にする。
6. copy、snapshot、delta undo の責務を分ける。delta undo の本番採用は別途 profile して判断する。
7. public field の private 化は最後に行い、必要な compatibility accessor を先に用意する。

完了条件:

- reachable state は各合法手後に invariant check を通る。
- editor state の許容範囲と拒否範囲が test で表現される。
- hash/cache の stale state を sanitizer/debug build で検出できる。
- copy 数、simulation/s、合法手生成速度が基準を下回らない。

### R3: rule query、validation、transition の整理

実施項目:

1. 予約元の解決、カード費用、宝石返却、貴族候補、ターン終了判定を小さな rule primitive として整理する。
2. `can_apply` は検査、transition は検査済み入力の更新として境界を明確にする。
3. 合法手 generator は必要な primitive を inline-friendly に利用し、汎用 object graph は導入しない。
4. generator、validator、transition の三者を random reachable state で differential test する。
5. 失敗時に部分更新が残る既存低レベル API は、その前提を明記して維持する。変更する場合は別の transactional API を追加する。

完了条件:

- 全 reachable state で生成手は validation を通る。
- 旧新の合法手 pack 列が完全一致する。
- apply 後の board、player、phase、hash が完全一致する。
- editor 状態の上限時にも既存の順序と prefix が一致する。

### R4: versioned encoder と feature schema の一元化

実施項目:

1. action space ごとに version、size、offset、対応 action type を記述する schema descriptor を導入する。
2. state feature にも version、shape、section offset、色順序を記述する。
3. C++ 実装を計算の正本にし、Python helper は native API へ委譲する。
4. Python に固有の invalid/fallback 動作は compatibility wrapper で明示的に維持する。
5. schema descriptor から documentation/test fixture を生成できる形を検討する。
6. gem 色 ID の方針が migration を伴う場合は、旧 schema を変更せず新 version を追加する。

完了条件:

- 48/V2/V3 の全 action ID が旧実装と一致する。
- random reachable/editor state の feature が bit-for-bit 一致する。
- Python と C++ で size/offset の独立した手書き定義が残らない。
- 既存モデル向け schema を無指定時の default として維持する。

### R5: MCTS の所有権と orchestration の整理

このフェーズの大規模変更は、parallel search plan の stable 化ゲート通過後に開始する。それまでは翻訳単位の分割など、trace と探索列が完全一致する機械的変更に限定する。

実施項目:

1. 旧 API を維持する facade、逐次 tree owner、parallel session controller、config validator を分ける。
2. worker lifecycle、bounded queue、virtual loss、result aggregation、error cleanup の所有者を明示する。
3. deterministic と throughput scheduler の重複は、同じ semantics が証明できる primitive のみ共通化する。
4. trace serialization/replay を探索制御から分離する。
5. compact edge、bitset、packed state は hot-path representation として維持する。
6. legacy batch API の deprecation 可否は downstream 利用状況を確認して別途決める。
7. 実 NN、品質 canary、seed variation、長時間 soak、追加 feature signature を stable 判定に含める。

完了条件:

- 旧新で同一 seed の選択列、trace bytes、root statistics、結果が一致する。
- throughput mode は race-free で、TSan と長時間 soak を通る。
- exception/cancel/worker failure 後に queue、virtual loss、thread が残らない。
- serial/parallel とも基準 NPS、latency、RSS を下回らない。

### R6: solver と puzzle tooling の分割

実施項目:

1. search limit、state key、action ordering、score/tie-break、terminal result の共通 value type を抽出する。
2. visible-only と reveal-verified に固有の semantics は無理に統合しない。
3. hidden outcome 列挙、oracle 判定、proof DAG 構築を個別 component に分ける。
4. 非テンプレートの C++ solver 実装を `.cpp` へ移す。
5. Python DFPN を core search、table、proof、output、CLI に分ける。
6. puzzle generator を候補生成、外部 engine adapter、検証、永続化に分ける。
7. 既存 script path と CLI option は薄い wrapper で維持する。

完了条件:

- fixture ごとの解、score、action order、proof DAG digest が一致する。
- timeout/node limit/cancel の境界結果が一致する。
- CLI の stdout/stderr、exit code、生成ファイル形式が一致する。
- benchmark の nodes/s と peak RSS が悪化しない。

### R7: Python application 層と外部 AI adapter の整理

実施項目:

1. FastAPI endpoint を薄くし、game/session/kifu/replay application service を分ける。
2. process-global dictionary を `SessionStore` / `KifuStore` interface の背後に置く。
3. replay の legacy pickle reader を隔離し、入力制限と unsafe-format 表示を明確にする。
4. USI/KIFU を tokenizer/parser、DTO、legal-action resolver、game builder、serializer に分ける。
5. USI 仕様自体は `usi` repository を正本とし、本リポジトリでは compatibility test を維持する。
6. AI を provider protocol 経由で呼び、model discovery、sibling path 操作、具体的な学習実装を外部 repository へ段階移行する。
7. `print` による診断を structured logging に置き換える。
8. AI 未導入時の既存 503 応答と response schema を維持する。

完了条件:

- endpoint response、status code、session/replay semantics が旧実装と一致する。
- engine package の通常 import では外部 repository や model file を探索しない。
- 外部 AI の有無を fake provider で再現できる。
- cross-repository 移行には双方の互換期間と rollback 手順がある。

### R8: テスト・文書・残存 debt の整理

実施項目:

1. phase/history 名のテストを domain/rules/encoding/mcts/solver/api の責務名へ整理する。
2. C++ の巨大 test executable を subsystem 単位に分割する。
3. shared fixture と property/differential test support を整備する。
4. C++ coverage をまず report-only で導入し、妥当な閾値を確認してから gate 化する。
5. `architecture.md`、`overview.md`、`engine_specs.md`、`tasks.md` を現行仕様へ更新する。
6. 第1次計画は完了記録として明記し、本計画の各フェーズへ変更記録をリンクする。

完了条件:

- 失敗した test から対象 subsystem と契約が分かる。
- 公開 header の単独 compile と主要 compatibility matrix が CI 上で確認できる。
- 文書の色順序、貴族処理、MCTS/parallel status が実装と一致する。

## 8. フェーズ別の検証マトリクス

| 変更領域 | 必須検証 |
|---|---|
| 文書・test support のみ | `pytest`、`py_compile`、関連 link/fixture check |
| binding/build | editable install、Python 全 test、native test、package/wheel、全 OS/arch CI |
| domain/rules/encoding | 上記 + 旧新全手同値、golden digest、ASan/UBSan、performance test |
| serial MCTS/solver | 上記 + seed replay、trace/proof 同値、NPS/RSS benchmark |
| parallel MCTS | 上記 + TSan、deterministic replay、failure injection、nightly soak、実 evaluator canary |
| Python Web/AI | API contract、session/replay、fake provider、package without optional dependencies |

各 PR では最低限、リポジトリ標準の次のコマンドを実行する。

```bash
pip install -e .
python -m pytest
python -m pytest -m performance
python -m py_compile csplendor/*.py
```

C++ または binding を変更した場合は native test、strict build、該当 sanitizer も実行する。ローカルにない OS/architecture は、PR 上の GitHub Actions を merge gate とする。

## 9. PR の分割方針

1 PR では一つの責務境界だけを変更し、機械的な移動と意味上の変更を混ぜない。

推奨する最初の実装順は次の通りである。

1. R0-A: 公開 API/不変条件表（完了）
2. R0-B: benchmark/build/include 基準値と共通 test support（完了）
3. R1-A: binding 登録関数の分割（挙動変更なし、完了）
4. R1-B: snapshot/trace 等の非テンプレート実装を `.cpp` 化
5. R2-A: invariant checker と cache invalidation の可視化
6. R3-A: rule primitive の一つを旧新 differential test 付きで抽出

MCTS、solver、Web/AI の大規模分割を同時に開始しない。基盤となる build/domain 境界を先に安定させ、それぞれ独立した benchmark と rollback 可能な PR にする。

各 PR に次を記録する。

- 変更前後の責務と依存方向
- 固定した公開契約
- 旧新同値テストの対象と結果
- benchmark の環境、seed、試行回数、中央値
- OS/architecture CI の結果
- compatibility wrapper の削除条件

## 10. 判断・停止ゲート

次の場合は自動的に「リファクタリング」として進めず、設計判断を行う。

- gem 名と数値 ID のどちらかを変更する必要がある。
- action ID、feature index、snapshot、USI、trace、proof digest が変わる。
- 合法手集合だけでなく順序や上限時 prefix が変わる。
- 失敗時 partial mutation を transactional に変える。
- public C++ field/header を互換 shim なしで削除する。
- Python object の参照/コピー、例外、fallback が変わる。
- parallel MCTS の stable 判定前に scheduler semantics を統合する。
- 外部 AI 実装を別 repository へ移す際の同時リリースが用意できない。
- benchmark が設定閾値を下回り、原因を説明できない。

この場合は、旧仕様を維持する path と、新 version/API を追加する path を比較し、移行・rollback 計画を先に文書化する。

## 11. 完了の定義

第2次リファクタリングは、単にファイルが小さくなった時点ではなく、次を満たした時点で完了とする。

- domain、rules、encoding、search、binding の依存方向が build target 上でも表現される。
- binding module 初期化が薄く、各 subsystem の登録と test が独立している。
- encoder/feature の schema と色順序に明確な正本と version がある。
- state profile、不変条件、mutation/cache invalidation の所有者が明確である。
- MCTS と solver の public facade から内部 tree/scheduler/proof 実装を変更できる。
- Web endpoint、session store、USI/replay、AI provider が責務単位で test できる。
- engine package の通常利用が sibling repository の配置に依存しない。
- 全互換テスト、sanitizer、platform CI、性能基準を継続して満たす。
- `doc/` の現行仕様と実装が一致し、第1次・第2次の変更履歴を追跡できる。

## 12. 次の着手点

gem 色 ID のcross-layer golden test、schema fingerprint、R0-Aの公開契約台帳、
状態不変条件表、R0-Bのbuild/include/runtime基準値と共通test supportは完了した。

R1-Aのbinding分割も完了し、module初期化、domain、rules、encoding、MCTS、solverの
翻訳単位を分離した。公開surfaceとruntime同値性を維持しつつ、登録source 1本の
incremental build時間とpeak RSSを削減した。

次の構造変更はR1-Bのsnapshot/trace等の非template実装の`.cpp`化とする。R1-Aで得た
登録境界とR0基準値を使い、対象を一つずつ独立PRで移す。
