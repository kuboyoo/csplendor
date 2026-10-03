# csplendor 単体高速化レビュー（2026-10-04）

対象: `feature/probabilistic-mate`（3cb4d1b）時点の `src/`・`csplendor/`。
コードは変更していない。読解・自作マイクロベンチ・既存ベンチ（`benchmark_engine_hotpaths`）による調査結果をまとめる。

## 0. 前提と方針

### 0.1 「仕様を変えない」の定義

GUI（splendorgui）と強化学習（dlsplendor）への影響を避けるため、次をすべて保つ案だけを「仕様不変」とした。

- Python API の名前・引数・戻り値の型、dtype、shape。
- 行動コード、V2/V3 行動ID、特徴量テンソル（ビット一致）、USI/KIFU 形式。
- 同一 seed での探索結果。RNG の消費順、tie-break、訪問数、教師分布を含む。
- 詰み探索の結果と証明。

ノード数などの統計だけが変わる案は「統計のみ変化」と区別した。詰み探索にはゴールデンテストがあるため、この区別が必要になる。
上記のどれかを変える案は §6 に分けた。

下流での使われ方は次のとおり確認した。

- dlsplendor・splendorgui は csplendor の C++ ヘッダを直接 include していない。利用は Python binding 経由だけである。
- C++ 構造体の内部レイアウトを変えても、binding の振る舞いを保てば下流には影響しない。

### 0.2 計測方法と注意

- `perf` は `kernel.perf_event_paranoid=4` のため使えなかった。代わりに、関数単位の自作マイクロベンチ（`-O3`、単スレッド、ウォームアップあり）と既存ベンチで内訳を分解した。
- 数値は単一マシン・単発計測の目安である。採否は、既存規約（`doc/csplendor_optimization_v2_post_phase3c.md` §15–16）に沿った A/B 測定で判断すること。
- 既に実装済み・棄却済みの施策は再提案していない。対象は R1–R4、R9、4B-1、5B-R、4C-1/2/3、5D などである。

## 1. 推奨順（要約）

| 順位 | ID | 内容 | 想定効果 | 仕様 | リスク |
|---:|---|---|---|---|---|
| 1 | D1 | 準詰み置換表の `float` 保存を `double` に変更（**結果に影響しうる不具合の疑い**） | 閾値0.9の再探索を除去 | 結果は不変。正しさは向上 | 低 |
| 2 | A1 | 合法手数え上げで返却量をスカラー計算する | `count_all_fixed` が 1.4〜2.5倍速 | 不変 | 低 |
| 3 | A2 | `PlayerState` の vector を固定長にして Board を trivially copyable にする | 中盤以降の複製が 40→約9 ns | 不変（binding はプロパティ化） | 中 |
| 4 | B1 | `V3SearchSession` の collect/apply を対局単位で並列化 | collect が数倍、全体で 1.5〜2.5倍（推定） | 既定スレッド数1なら不変 | 中 |
| 5 | A3 | ビット一致の遅延 MT19937 | determinize 1回あたり約0.5µs減 | 不変 | 低〜中 |
| 6 | C1 | 公開カード統計の vector 確保と sort を除去 | 1葉あたり約0.6〜0.8µs | 不変 | 低 |
| 7 | C2 | V3 合法ID・特徴量の一括 native API を追加 | Python 境界で数十µs/局面 | 追加のみ | 低 |
| 8 | C3 | V3 支払いコーデックを表引きにする（R10a の前倒し） | 購入手1件あたり〜1µs → 数十ns | 不変 | 低 |
| 9 | D2 | 準詰み `weighted_outcomes` をキャッシュし O(D) にする | 準詰みの1ノード固定費を削減 | 不変 | 低 |
| 10 | A4 | `apply` で未使用の `Board previous` を構築しない | `apply` 約44ns のうち約5ns | 不変 | 低 |

B3（V3 合法手の cache）、M2〜M4（共有木の atomic・lock）など、その他の項目は各章に記載した。

---

## A. ルールエンジン中核（game / board / move_generator）

### A1. 合法手の数え上げ・選択でトークン総数をスカラー計算する【高】

- **場所**: `src/move_generator.h:588`（`count_with_returns`）、`:527`（`emit_with_returns_impl`）。下請けは `src/rule_query.h:80`（`gems_after_token_action`）。
- **現状**: 基本手1件ごとに次の処理をしている。
  - `std::array<uint8_t,6>` を複製し、`take[]` をバイト単位で加算する。
  - その後 `token_total` で6バイトを合計する。

  直前にバイト単位で書いた `Action` を幅の広いロードで読み直すため、store-forwarding の停止が起きていると推定する。
- **実測**（初期局面、合法手30）:

  | 処理 | 時間 |
  |---|---:|
  | 基本手の列挙のみ | 44 ns |
  | `count_all_fixed` | 292〜348 ns |

  差のほとんどが、返却が発生しない局面での `gems_after_token_action` と `required_token_return` である。
- **提案**: 手番プレイヤーの所持トークン総数を、列挙の開始時に1回だけ計算する。各手では `excess = base_total + Σtake`（予約なら `+1`）をスカラーで求める。
  - `excess <= 0` なら、配列を作らずに件数 1 を返す。
  - 返却が必要な場合だけ、従来どおり配列を作る。
- **試作の結果**（同じ判定ロジック、件数は全局面で一致）:

  | 手数 | 現行 | 試作 |
  |---:|---:|---:|
  | 0手目 | 319 ns | 126 ns |
  | 12手目 | 227 ns | 97 ns |
  | 30手目 | 96 ns | 70 ns |
  | 60手目 | 174 ns | 103 ns |

- **波及**: 同じ処理は `select_all_capped`（ランダム手の選択）、`emit_with_returns_impl`（`legal_action_codes`、V2/V3 mask）、`count_all_fixed` にある。ランダムプレイアウトは count と select の2パスを通るため、両方に効く。
- **仕様**: 出力順・件数・コードは不変。
- **追加案**: `emit_purchase_options`（`:456`）は、`min_gold` の判定より前に `effective_card_cost` を計算している。判定を先にすれば、買えないカードでの無駄な計算を省ける。

### A2. Board を trivially copyable にする（`purchased_cards`/`acquired_nobles` の固定長化）【高〜中】

- **場所**: `src/player.h:83-84`。`std::vector<uint8_t>` を2本持つ。
- **現状**: `Board` は trivially copyable ではない（`std::is_trivially_copyable<Board>` = 0）。複製のたびに最大4回のヒープ確保と解放が起きる。これは次の処理すべてに影響する。
  - `clone_light`、`shuffled_clone`
  - PASS 時の `Board next = board`
  - `record_history` の退避
  - V3 rollout の `clone_light`、ソルバの blank probe
- **実測**（`clone_light`）:

  | 状態 | 時間 |
  |---|---:|
  | 購入なし | 14.5 ns |
  | 各12枚購入・貴族2枚 | 40.3 ns |
  | 同じサイズの trivially copyable 構造体 | 8.8 ns |

  マルチスレッドでは allocator の競合も加わる。
  - なお、MCTS・ソルバ担当のレビューでは「Board は POD でヒープを持たない」とされていた。実測によりこれは誤りと判断した（§7）。
- **提案**:
  - `FixedStack<uint8_t, CARD_COUNT>` と `FixedStack<uint8_t, NOBLE_COUNT>` に置き換える（約100B増）。
  - binding（`src/bindings_domain.cpp:78-79`）は `def_readwrite` から `def_property` に変える。getter は `list` を返し、setter は `list` を受け取る。
  - これで Python からは同じに見える。現状でも getter は複製を返すので、`append` が盤面に反映されない挙動まで同じになる。
- **仕様**: Python からは不変。ただし容量（90/10）を超える列を代入すると例外になる。現行ではルール上ありえない入力なので、editor の検証方針に合わせる。
- **注意**:
  - `game_snapshot.cpp`、`undo_record.h`、`solver_normal_rollback.h` など、内部で vector API を使う箇所（約70箇所）の追従が必要。
  - これが入ると「R7 / 5B-R（Game scratch 再利用）」の前提が変わる。複製そのものが安くなるため、scratch 再利用は不要になる見込み。

### A3. ビット一致の遅延 MT19937【中】

- **場所**: `src/board.h:822`（`randomize_hidden_information`）。V3 探索の毎シミュレーション（`mcts_v3.h:263`）、rollout（`:1250`）、legacy の determinization で使われる。
- **現状**: 毎回 `std::mt19937(seed)` を構築している。624語の seed 計算と、初回の全 twist が走る。実測で 1.09〜1.11 µs。shuffle が消費する乱数は数十〜百個程度しかない。
- **提案**: 出力 i（i < 227）は、`mt[i]`、`mt[i+1]`、`mt[i+397]` だけで決まる。そこで、seed の漸化式を必要な位置まで、twist を出力した分だけ計算する。
  - 227個を超えたら、`std::mt19937` を生成して `discard` で同じ位置まで進め、そちらに切り替える。
  - shuffle 関数には、テンプレートで UniformRandomBitGenerator として渡す。
- **試作の結果**（`std::mt19937` と最初の1500出力が一致）:

  | 消費数 | `std::mt19937` | 遅延版 |
  |---:|---:|---:|
  | 40 | 1112 ns | 563 ns |
  | 90 | 1161 ns | 688 ns |
  | 200 | 1235 ns | 970 ns |

  V3 では1シミュレーション（約8.4µs、MCTS 担当の計測）の約6%に相当する。
- **仕様**: 数列は標準で定義されているため不変。全 seed 境界での oracle テストを必須とする。

### A4. `apply` で使わない `Board previous` を構築しない【低】

- **場所**: `src/game.h:294`。
- **現状**: `record_history=false`（探索は常にこちら）でも、`Board previous;` をデフォルト構築・破棄している。実測 5.2 ns で、`apply_only` 約44 ns の約1割にあたる。
- **提案**: 次のどちらかにする。
  - `std::optional<Board>` にして、`record_history` のときだけ構築する。
  - 退避処理を分岐の中に移す。
- **仕様**: 不変。

### A5. その他（低）

- `Game::legal_action_codes`（`game.h:72`）は、16KB の stack scratch から vector へコピーしている。件数が分かっているなら、`count_all_fixed` で reserve して直接書き込むほうが速い可能性がある。ただし数え上げを2回行うことになる。A1 の後に再計測して判断する。
- `observable_hash`（`board.h:687`）は約21 ns で毎回全体を計算している。差分更新は `apply` 全体のコストを上げるため、現時点では推奨しない。

---

## B. MCTS（V3 / legacy / 共有木）

`mcts_v3.h`（V3SearchSession）は既存の最適化フェーズより後に追加されたため、まだ最適化されていない。
MCTS 担当の簡易計測では、Python から乱数 prior で回して約8.8万 sim/s だった。dlsplendor の実測は約6.5万 sim/s なので、**V3 ではエンジン側が主要なコスト**と推定する。

1シミュレーションの内訳（約8.4µs）:

| 処理 | 1回あたり |
|---|---:|
| `shuffled_clone` | 1.25 µs |
| `for_each_legal_with_id` | 0.67 µs（降下の1ステップごと） |
| `encode_public_card_statistics` | 1.79 µs |

### B1. `V3SearchSession` の collect/apply を対局単位で並列化【高】

- **場所**: `src/mcts_v3.h:1444-1457`（collect）、`:1461-1477`（apply）。
- **現状**: GIL は解放しているが、全対局を1スレッドで順に処理している（確認済み）。各 `GameSearch` は RNG・木・scratch を自前で持ち、互いに独立である。
- **提案**:
  - 対局ごとに局所の Batch/pending/Stats を作って並列に処理し、最後に **slot 順に連結**する。
  - apply は slot ごとの連続区間を並列に処理し、区間内の行順は保つ。
  - `Config` にスレッド数を追加する（既定 1）。
- **仕様**: 既定値では完全に不変。スレッド数を増やしても、行順・RNG・木はビット一致するはず。これを differential テストで確認する。
- **リスク**: 中。例外は最小 slot のものを再送出する。`reset_game` などとの排他を守る。

### B2. 公開カード統計の確保と sort を除去 → C1 と同じ

### B3. 観測者の手番ノードで V3 合法手リストを cache する【中】

- **場所**: `mcts_v3.h:317-334`、`818-829`。
- **提案**: 情報集合木では、観測者の手番ノードの合法手は world に依存しない見込みである。そこで、生成順を保った `LegalAction` 列と edge index をノードに保持する。`determinization=false` なら全ノードで cache できる。
- **前提**: 「world に依存しない」ことの証明が必要。cache と再列挙を比較する VERIFY ビルドを必須とする。メモリは増える。
- **効果**: 1シミュレーションの約10〜15%（推定）。

### B4. 共有木（parallel）の atomic・lock【中】

- **M2（R5a の再検討）**: `mcts_concurrent_tree.h:610-613`、`679-682` の `access_epoch.fetch_add` と `last_access.store` は書き込みだけで、どこからも読まれていない（grep で確認）。全スレッド共有の RMW なので削除できる。仕様は不変。
- **M3（R8c）**: `with_node_lock`（`:146-149`）で、毎回 `weak_ptr::lock()` による control block の atomic 増減をしている。`owner_before` による同一性比較に置き換えられる。4C-3（約4%）と合わせれば 5% 基準を超える可能性がある。
- **M4（R5b）**: 選択時の `find_edge`（`:782-811`、`871`、`918`）は `lower_bound` を重複して呼んでいる。world mask と edges を1回の merge 走査にまとめる。

### B5. legacy・その他（低）

- **R6b**: `mcts_orchestration.h:48`、`136`、`146` で同じ key を何度も lookup している。`LegacyTreeRecord&` を内部で使い回す。LRU の touch 回数と順序は保つこと。
- `mcts_v3.h:266`、`300-301`: `path` の伸長による realloc と PendingLeaf へのコピーがある。reserve と `std::move` で解消できる。
- `mcts_v3.h:337-392`: 同じ `chosen_id` を最大5回 `find` している。
- `mcts_v3.h:852-868`: `select_grouped` が O(L×G) の線形探索になっている。
- `mcts_v3.h:1447`: `features` を reserve していない。

---

## C. エンコード・binding・Python 層

dlsplendor の実際の呼び出し方（`../dlsplendor` を grep で確認）:

- `network/encoder.py:63-90`: `encode_canonical` と `encode_public_card_statistics` の list 版を呼び、`np.asarray` と `concatenate` をしている。
- `network/action_encoder.py:37`: `legal_actions` を Python でループし、`ActionEncoderV3.encode` を1件ずつ呼んでいる。

### C1. `encode_public_card_statistics` の確保と sort を除去【高】

- **場所**: `src/state_encoder.h:194-196`、`src/board.h:802`（`observable_card_pool`）。
- **現状**: tier ごとに `std::vector` の確保と `std::sort` をしている（確認済み）。
- **提案**: deck と相手の非公開予約から、カードIDのビット集合（90bit）を作り、昇順に走査する。
  - 走査順がソート後の順序と同じになるため、`float` の加算順（`efficiency_sum` など）も保たれ、ビット一致する。
  - `observable_card_pool` の公開 API はそのまま残す。
- **効果**: 1葉あたり約0.6〜0.8µs。V3 探索と dlsplendor の特徴量経路の両方に効く。

### C2. 一括 native API を追加する【高・追加のみ】

既存 API は変えず、別名で追加する。

1. `ActionEncoderV3.legal_action_ids(game) -> np.ndarray[int32]` を追加する。
   - 中身は既存の `for_each_legal_with_id`（`action_encoder_v3.h:445`）を1回走査するだけ。
   - これで N 件の `Action` 生成、N 回の境界越え、重複チェックがなくなる。
   - forced pass の扱いは既存の mask と揃える。
2. 特徴量の numpy 版、または書き込み先指定版を追加する。
   - 例: `encode_v3_input(game, player, observer, ..., out=None)`。
   - `mcts_v3.h` の `encode_leaf` と同じ連結を native で行う。
   - list 変換と `concatenate` を省ける。
- dlsplendor 側の呼び出しの差し替えと組み合わせる必要がある。

### C3. V3 支払いコーデックを表引きにする（R10a の前倒し）【中〜高】

- **場所**: `src/action_encoder_v3.h:127-214`。`count_compositions` が再帰で、encode/decode 1回あたり数百回呼ばれる（確認済み）。
- 旧文書では「48手 MCTS には効かない」とされていた。しかし `for_each_legal_with_id` を通じて **V3SearchSession のホットパスにも入っている**。
- **提案**: constexpr の `suffix_count[card][pos][sum]` 表を作り、旧関数は oracle として残す。
- **仕様**: rank/unrank の結果は同一。

### C4. スナップショット復元の無駄な初期化【中】

- **場所**: `src/game_snapshot.cpp:368` の `Game game(0);`（確認済み）。
- **現状**: `board.init` を通るため、mt19937 の seed 計算（約1.1µs）と90枚の shuffle をしてから、Board 全体を上書きしている。
- **提案**: `Game(NoInit)` 相当の内部 factory を使う。あわせて、binding（`bindings_rules.cpp:120`）での `std::string` へのコピーと、serialize 側の reserve 不足も直す。
- **仕様**: 出力バイト列は不変。dlsplendor の archive 復元に効く。

### C5. 低

- V2/V3 `get_action_mask` と `owning_array_copy` は、stack 上で0初期化した配列を作ってから numpy にコピーしている。numpy のバッファへ直接書き込める。
- `encode_board` は、全体を0初期化したあとで canonical の swap をしている。R10b と同時に見直す。
- `csplendor/action_space.py:103`: `np.asarray(uint8, dtype=bool)` はコピーを作る。`.view(np.bool_)` にすれば値は同一でコピーが不要。

### C6. API 層（GUI の応答時間のみ。RL には無関係）

- USI を解決するたびに、合法手を全件 `action_to_usi` で文字列化している。購入手ごとに `board.players`（PlayerState 2人分の複製）も取得している。分岐・replay では初手からこれを繰り返すため、O(手数×合法手) になる。
  - 該当箇所: `usi_resolver.py:21`、`game_service.py:161-167`、`kifu_service.py:111-114,191-198`、`usi_kifu.py:490-494`。
  - 改善案: 先に種別と card_id で候補を絞り込む。完全一致を優先する順序と、index 順の tie-break は保つ。
- `ai_manager.py:620-630,722`: ログレベルに関係なく合法手の一覧を文字列化している。`isEnabledFor(DEBUG)` で囲む。

---

## D. 詰み探索・準詰み

### D1. 準詰み置換表の `float` 保存【最優先・正しさに関わる疑い】

- **場所**: `src/reveal_verified_solver.cpp:632`（`ProbBounds{float lo, hi}`）、参照は `:2025-2037`、保存は `:2083-2105`。
- **現状**: double の探索値を float に丸めて保存している。0.9 は float では `0.8999999762` になる。
  1. **速度**: 閾値の窓は `(T-1e-6, T)`（`csplendor/near_mate.py:105-106`）である。「≥0.9 が証明済み」の局面でも `bounds.lo >= beta` が偽になり、部分木を探索し直す。
  2. **正しさの疑い**: チャンスノードの内部では、子ノードの窓が平均化によって変換される。このため、値 0.9 が正確値（`lo = hi`）として保存される経路がありうる。再ヒット時には `0.8999999762` がそのまま返る（`:2027-2028`）。その結果、親の平均が閾値を下回り、根で `value >= threshold` が偽、つまり**本来は証明できる局面を「反証」と判定する**可能性がある。
     - コード上で経路は確認したが、実際に再現する局面はまだ用意していない。
- **提案**: `lo`/`hi` を `double` にする（1エントリあたり8B増）。メモリを抑えたい場合は、`lo` を切り下げ・`hi` を切り上げで丸め、返す値は元の double を使う。
- **仕様**: 判定は「正しい方向に」変わる可能性がある。node 数などの統計は変わる。準詰みには統計のゴールデンテストはない。
- **最初にやること**: 0.9 や 0.7 などの閾値で、float 版と double 版の判定を比べる differential テストを作り、不一致の有無を確認する。

### D2. `weighted_outcomes` を手ごとに作り直している【高】

- **場所**: `reveal_verified_solver.cpp:1917-1955`（呼び出しは `:1963`）。
- **現状**: 手1つを評価するたびに、`std::vector` を確保する。さらに、山札の各カードについて `same_card_equivalence_tuple` で全 class と線形比較している。
- **提案**: `card_equivalence_class()` の表を使って O(D) にする。局面ごとに3山ぶんを遅延計算し、scratch にキャッシュする。最初に出たカードを代表とする規則は保てる。
- **仕様**: 不変。

### D3. ノードごとの一時 vector【中】

- `ordinary_ordered_actions`（`:1533-1545`）は、`legal_action_codes()` の vector を毎回確保している。準詰みの `win_probability`（`:2043`）、`filter_probe_actions`（`:1781-1825`）、`apply_order_hints`（`:1845-1890`）は scratch を使っていない。
- **提案**: generator の sink から scratch へ直接書き込む。`filter` は card_id を添字とする固定長配列にする。hint 表は CSR 形式のフラット表にする。いずれも順序は不変。
- `visible_only_solver.cpp:350-379` の `representative_actions` は、ノードごとに `unordered_map` を構築している。(子局面のキー, 手) の組を scratch に溜め、sort してから重複を除けば、最終的な並びを保てる。
- 準詰みの置換表の保存（`:2093-2104`）は、同じキーで最大3回 `find` している。`try_emplace` 1回にまとめる。

---

## 6. 仕様変更を伴うため別扱いにする案

効果は大きいが、§0.1 のいずれかを変える。導入する場合は、オプトインのフラグか major 版での変更とすること。

| 案 | 効果 | 変わるもの |
|---|---|---|
| determinization を `PortableRng` に切り替える（`shuffled_clone_portable` は約0.3µs、`std::mt19937` 版は約1.2µs） | 1シミュレーションあたり約0.9µs減 | 探索結果（RNG 列） |
| 準詰みの閾値ラダーで置換表を持ち越す（`near_mate.py:85-107`、`prob_memo_.clear()` `:2118`） | 深さ掃引全体で数倍の可能性 | 統計、fail-soft の値。経路依存の値（繰り返しを0とする扱い）も置換表に入るため、判定がずれる余地もある |
| 準詰みで最終ラウンドの直接解決と、めくれの並べ替え | 中〜大 | 統計、`best_action_reveals` の順序 |
| 深さ掃引で置換表を再利用（`mate_depth.py:773`） | 中 | 統計、手順、証明 DAG。既存の `MateSearchSession` を使ってもらう運用で代替する |
| `Board.players` を参照で返す | binding の複製を削減 | aliasing の挙動。推奨しない |
| legacy `prepare_batch_simulations` で GIL を解放する | 並列実行の余地 | スレッド安全性の契約 |
| virtual loss の合計を整数化する | 共有木の走査を削減 | 浮動小数の結果 |

## 7. 検証の過程で訂正した点

- MCTS 担当とソルバ担当のレビューでは「Board/Game は POD で、ヒープ確保なしにコピーできる」とされていた。しかし `PlayerState` が `std::vector` を2本持つため、実際には trivially copyable ではない。購入が進んだ局面では、`clone_light` が約40 ns かかり、うち約30 ns がヒープ処理だった（A2）。
- 遅延 MT19937 は、最初の試作でフォールバック用の engine を毎回構築してしまうバグがあり、「遅い」と誤判定していた。修正後の再計測で 1.6〜2倍速いことを確認した（A3）。

## 8. 進め方の提案

1. **D1** の differential テストで、まず不一致の有無を確かめる。不一致があれば不具合修正として先に入れる。
2. **A1・A4・C1・C4・D2** は局所的でビット一致を検証しやすい。既存のフラグ方式（OFF/ON でコードを同一に保つ `volatile` 方式）で1件ずつ入れる。
3. **A3** は `std::mt19937` との oracle テストを付けて入れる。
4. **A2** は binding 互換のテスト（list の代入・取得、snapshot の往復、undo）を先に用意してから入れる。
5. **B1・C2・C3** は、dlsplendor 側の呼び出しの差し替えと合わせて、実際の自己対局スループットで評価する。
6. **B3・B4** は VERIFY ビルドと TSan を前提に、既存の 5% 採用基準で判断する。

計測に使った自作マイクロベンチは、セッションのスクラッチ領域に置いた（repo には含めない）。

---

## 9. 実施結果（2026-10-04、ブランチ `perf/hotspot-optimizations`）

上記のレビューを受けて、効果の大きい順に実装した。
採用したのは、実測で効果を確認でき、かつテストが通ったものだけである。
効果がなかったもの、または公開仕様に反するものは採用せずに差し戻した。

### 9.1 採用した変更

| コミット | 内容 | 主な効果（main比、結果はビット一致） |
|---|---|---|
| A1 | 合法手の数え上げ・選択で、返却量をスカラー計算する | `legal_count` 最大1.87倍、`legal_select` 最大1.91倍、ランダム自己対局 1.14倍 |
| A3 | `std::mt19937` とビット一致する遅延 seed・遅延 twist の `LazyMt19937` | determinization の乱数初期化 約1.1µs → 約0.6µs |
| C1 | 公開カード統計を、ビット集合による列挙と SWAR でヒープ確保なしに計算する | 関数単体 約850 → 630ns |
| V3-1 | 3色取りの番号を表引きにする／辺検索を前回位置から前方走査する／辺位置を再利用する／経路バッファを再利用する | V3 探索のホットパス |
| C3 | 返却・支払いコーデックを constexpr の表引きにする | 支払い encode 9.1倍、decode 11倍、V3 合法手マスク 最大1.34倍 |
| C4 | スナップショット復元で、初期配牌と bytes のコピーを省く | `deserialize_snapshot` 1.96µs → 0.62µs（3.2倍） |
| D2 | 準詰みのめくれ結果を等価クラス表で集約する | 準詰み 約5〜8%（値・ノード数は完全一致） |
| B1 | `V3SearchConfig.num_threads`（既定1）を追加し、対局単位で並列化する | 8スレッド 4.4倍、16スレッド 4.9倍（1スレッド比） |
| C2 | `ActionEncoderV3.legal_action_ids(game)` を追加する（既存 API は変更なし） | Python で1手ずつ encode する場合の 14.1µs → 0.72µs/局面（約20倍） |

### 9.2 総合的な効果

- **V3 探索**（`V3SearchSession`、48局×400シミュレーション、Python から呼び出し、学習ジョブ並走下）は、構成ごとに 1.31〜1.43倍 になった。

  | 構成 | main | 本ブランチ | 倍率 |
  |---|---:|---:|---:|
  | det | 11.2万 sim/s | 15.7万 | 1.41 |
  | det+groups | 11.3万 | 16.2万 | 1.43 |
  | nodet | 11.7万 | 15.3万 | 1.31 |
  | rollout | 16.2万 | 22.9万 | 1.42 |

  すべての構成で、全バッチの特徴量・合法手ID・根の訪問数と行動価値のダイジェストが main と一致した。
- **64局ベンチでのスレッド並列**:

  | 条件 | sim/s | main比 |
  |---|---:|---:|
  | main（逐次） | 9.96万 | 1.0 |
  | 1スレッド | 16.3万 | 1.63 |
  | 2スレッド | 27.4万 | 2.75 |
  | 4スレッド | 46.5万 | 4.67 |
  | 8スレッド | 71.7万 | 7.20 |
  | 16スレッド | 79.8万 | 8.02 |

  ダイジェストはすべて main と一致した。16スレッドで伸びが鈍るのは、学習ジョブとのコア共有と、collect/apply 以外の逐次部分によるものと考えられる。
- **詰み探索**: 厳密詰みはほぼ同等で、準詰みは約5%速くなった。どちらも結果のシグネチャは main と一致した。
- **従来型 MCTS・共有木・可視限定ソルバ**: 劣化なし（±1%）。可視限定ソルバの reveal_heavy は 1.08倍。

### 9.3 検証

- C++ ネイティブテスト 46件がすべて合格した。新規に次の2件を追加した。
  - `fastpath_equivalence_unit`: 遅延 MT・スカラー計算・カードプールの oracle 比較、および main 時点の公開カード統計の実装との float のビット比較。
  - `v3_session_parallel_unit`: スレッド数 1/2/4/16 で、全バッチと木がビット一致することを検査する。
- `v3_session_parallel_unit` を ThreadSanitizer 付きで実行し、データ競合はなかった。
- Python テスト 668件がすべて合格した（`-W error`、スキップ0件。usi リポジトリと `generated/` を参照する2件も含む）。
- `python -m py_compile csplendor/*.py` が通った。
- コーデックの表はすべて、旧関数から constexpr で生成している。全入力での一致は `static_assert` で保証した。

### 9.4 採用しなかったもの（実測・仕様確認の結果）

| 案 | 判断の理由 |
|---|---|
| A4 `apply` の `Board previous` を `std::optional` にする | 逆に約1割遅くなった（`apply_exact_hash` で 53 → 60ns） |
| A2 Board を trivially copyable にする | 固定長版は準詰み +15%、`purchase_apply` +30% だった。しかし `tests/test_domain_storage_contracts.py` が定める「来歴リストは長さ無制限」という公開契約に反する。契約を守る「インライン＋ヒープ退避」版では効果が数%にとどまり、一部は劣化したため見送った |
| M2 共有木の `access_epoch` 更新を削除する | 8/16スレッドで有意な改善がなく、ノイズの範囲だったため差し戻した |
| ソルバの合法手コードを一時 vector なしで流す | ±2〜3% で、ワークロードによって符号が逆転したため見送った |
| D1 準詰み置換表の `double` 化 | 600問の差分検証で、判定・ノード数ともに変わらず、速度の効果はなかった。値は7問で float の丸め誤差分だけ変わる（例: 0.9939393997 → 164/165）。精度の改善としては有効だが、返り値が変わるため、本ブランチの「仕様不変の高速化」には含めない。別途の不具合修正として検討することを推奨する |
| 公開カード統計の効率値を表引きにする | 効果がなかった |
| R6b 従来型 MCTS の重複検索をまとめる | 下流（dlsplendor/GUI）が従来型 MCTS を使っていないため、優先度が低いと判断して見送った |

### 9.5 下流への反映について

- dlsplendor は、このリポジトリを editable で読み込んでいる。本変更は、拡張モジュールを再ビルドした時点で反映される。
- 学習を実行中の場合は、その途中で `.so` を差し替えないこと。
- `num_threads` と `legal_action_ids` は追加 API なので、下流で使うかどうかは任意である。
  - dlsplendor の `ActionEncoder.legal_action_ids` は、`csplendor.ActionEncoderV3.legal_action_ids(game).tolist()` に置き換えられる（重複検査は別途必要）。
  - `--native-processes` で複数プロセスを使う場合は、`num_threads` を増やすとコアを過剰に割り当てることになる。プロセス数との兼ね合いで決めること。
