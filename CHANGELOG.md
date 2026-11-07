# 変更履歴 / Changelog

## 未出荷：詰み探索の最終手の直接生成（2026-11-07）

- 最終手カットの節点（最終ラウンドでない局面で攻め側の手番、残り1手）で、全合法手を作ってから 15 点に届く手に絞るのを
  やめ、15 点に届く購入（予約札・公開札）の全支払いパターンを合法手生成と同じ列挙（`MoveGenerator::emit_card_purchases`）
  で直接作る（`for_each_winning_last_move`、`doc/mate_usage.md` の「最終手の直接生成」）。貴族選択待ち・返却待ちと、
  購入以外の手でも 15 点に届く節点は従来どおり全手を作って絞る。作られる手の集合と順序は従来と同一で、判定・証明 DAG・
  主手順・ノード数・`legal_moves`・golden digest は変わらない。適用先はめくれ検証の解決器（厳密・blank）と準詰み。
- C++ 単体テスト（`tests/solver_components_unit.cpp`）で、ランダム局面 1,000 以上について直接生成と「全手を作って絞る」
  結果が一致することを確かめる。
- 計測（1 スレッド、置換表 200万件、47791b9 との比較）: BENCH の厳密探索は深さ4 が 2.07 → 1.73 秒（ノード数は同じ
  150.9万）、深さ5 の 60 秒で訪問できたノードは 4,453万 → 5,597万（+26%）。ランダム終盤 360 + 1,164 ケースで判定・
  ノード数・主手順が全て一致。

English: generates the attacker's winning last moves directly (the purchases
that reach 15 points, with the move generator's payment patterns) instead of
generating every legal move and filtering. Same move set and order, so every
result is unchanged; BENCH depth 4 runs in 1.73 s instead of 2.07 s.

## 未出荷：詰み探索の守り側の最終応手カット（2026-11-07）

- 守り側の手番で攻め側の残りが1手の節点（最終ラウンドでなく、守り側に貴族選択・返却が残っていない）で、守り側がどう応じて
  も攻め側が次の手番で勝つことを応手を作らずに判定し、成り立てば PROVEN にする（`doc/mate_usage.md` の「守り側の最終応手
  カット」）。判定は、守り側の1手で変わらない攻め側の持ち物（トークン・ボーナス・予約札）から保証できる手番終わりの点
  S\*（守り側は公開札を高々1枚、貴族を高々1人しか取れない）と、守り側の1手・2手の到達点の上界 D1・D2（銀行を無視した
  取得、補充が起こりうる段の全未知札、貴族2人）の比較で、結果は厳密なまま。厳密探索と blank 探索の両方に入れ、証明 DAG は
  このカットで証明された節点の応手を列挙して個別に証明し、主手順は代表の応手を進んでその先を一時的な表で探索して続ける。
  統計 `last_reply_cut_attempts` / `last_reply_cut_proofs` を足した（golden digest はこの2キーの追加だけで変わり、他の
  全フィールドは同じ）。
- 層ごとの計測（BENCH 深さ5）で、残り攻め手数 1 以下の層が時間の 93% を占め、守り側 d=1 の証明される節点（展開の 10%）が
  その子（攻め側 d=1 の 75%、守り側 d=0 の大半）を生むことが分かったのが根拠。
- 計測: BENCH の厳密探索は深さ4 が 233.8万 → 150.9万ノード・3.4 → 1.9 秒、深さ5 は 240 秒では両方とも未決。ランダム終盤
  360 ケースで判定の食い違いなし、未決 5 → 0、合計ノード 128.1万 → 40.1万。
- `tests/test_mate_blank_dominance_cut.py` の blank 意味論の数え上げは、守り側の応手が列挙されることを前提にしていたので、
  カットが成立しない守り側の脅威（銀行が空で実際には買えない札）を fixture に足した。`tests/test_root_mate_probe.py` の
  厳密な終盤探索の参照テストは、blank 探索が同じ予算で詰みを出すようになったので終盤予算を 2,000 → 1,000 ノードにした。

English: adds the defender's last-reply cut to the reveal-verified mate
search: a defender node with one attacker move left is proven without
generating the replies when the attacker's guaranteed turn-end score (from
cards the defender cannot take away) beats upper bounds on the defender's
one- and two-move reach. Exact results are unchanged; BENCH depth 4 runs in
1.9 s instead of 3.4 s and random endgames use 31% of the nodes.

## 未出荷：詰み探索のめくれ節点の blank 支配カットと blank 意味論の修正（2026-11-07）

- 厳密探索（`exact_reveal_search=True`）のめくれ節点で、最初の具体めくれを証明したあと、残りのめくれを blank の子局面
  1回の探索でまとめて証明する（blank 意味論は守り側に多く、攻め側に少なく許すので、blank で PROVEN なら全ての具体
  めくれで PROVEN。`doc/mate_usage.md` の「blank 意味論の支配」）。blank の子が証明できなければ従来どおり列挙する。
  主手順は最初の具体めくれを通る。証明 DAG・frontier・root 分割・準詰みは使わない。統計 `blank_cut_attempts` /
  `blank_cut_proofs` を足した（golden digest はこの2キーの追加だけで変わり、他の全フィールドは同じ）。
  計測: BENCH 局面の exact 深さ3 が 32,609 → 11,845 ノード、深さ4 が 240 秒・3.87億ノードで未決 → 242万ノード・3.5 秒で
  不詰みを確定。ランダム終盤 360 ケースで判定の食い違いなし、未決 31 → 5、合計ノード 558万 → 128万。
- blank 意味論（公開札だけの探索・anytime の正の証明）の山札を実際と一致させた（P3）: 山札予約の候補に blank の枡へ
  消費された札を含め（配列にない候補は山札の一番上と入れ替えて予約する）、oracle 手（blank の枡の札の購入・予約）は
  実際の補充と同じく山札を1枚消費する。これまでは消費された札が候補から落ち、oracle 手の後に山札の枚数が実際より
  多くなり、oracle で取った札が配列に残って山札予約の候補が空になる（探索が未決で止まる）ことがあった。
- blank の置換表は探索ごとに捨てる（`MateSearchSession` が持ち越すのは厳密表だけ）。

English: adds the blank dominance cut to the exact reveal-verified search
(after the first concrete reveal of a refill is proven, one blank/oracle
search covers the remaining reveals), and makes the blank semantics' deck
faithful (deck-reserve candidates include cards consumed into blank slots;
oracle moves consume a deck card).

## 未出荷：詰み探索の最終手カット（2026-11-07）

- 最終ラウンドでない局面で攻め側の残りが1手のとき、手番の終わりに15点に届く手（購入＋貴族、`VISIT_NOBLE`、
  すでに貴族の条件を満たしている場合のその他の手）だけを展開し、無ければ手を作らずに反証する。めくれ検証・
  visible-only・準詰みの各探索に入れた。結果は厳密なまま。BENCH 局面では exact 深さ3 が 133万 → 3.3万ノード、
  visible-only の5手詰めが 149万 → 36万ノード、準詰み深さ2 が 300万ノードで未決 → 1,140 ノードで決着。
  ランダム終盤 360 ケースで判定は変わらない。点数の計算は `src/solver_win_threshold.h` にまとめた。

English: adds the exact last-move cut (the attacker's last move outside the
final round must end the turn at 15+ points) to the reveal-verified,
visible-only and near-mate searches.

## 未出荷：詰み探索の不具合3件の修正と置換表の既定上限（2026-11-07）

- visible-only ソルバー（`solve_visible_only_winner_cpp`、`--reveal-verified` の候補探索）が、反復窓が
  8局面を超える局面からの探索で PASS に出会うと `abort()` していたのを直した。`RepetitionWindow` の代入は
  確保済みの領域を縮めない。
- 貴族選択（`VISIT_NOBLE`）で攻め側の手番が終わるとき、root の並列分割・`expand_mate_frontier`・証明 DAG の
  深さが減っていなかったのを直した。貴族選択待ちの root から `jobs > 1` の深さ掃引が詰み手数を1手短く
  報告していた。深さの規則は探索本体と同じ1か所（`child_depth`）にまとめた。player1 が攻め側で最終ラウンドの
  貴族選択を通る証明 DAG が `complete=False` になっていたのも同じ規則で直った。
- `NativeMateSearchSession` で、攻め手を削る探索（`no_take_probe`、`attacker_restrict_top_k > 0`）の反証が
  持ち越され、その後の全手の探索が詰みを見逃していたのを直した。制限つきの探索の後は反証を表から除く。
- 詰み・準詰みの置換表の上限 `max_memo_states` の既定を 0（無制限）から `DEFAULT_MAX_MEMO_STATES`
  （200万件、約 180 MB）に変えた。0 を渡すと従来どおり無制限。session が持ち越す表は探索前に上限の半分以下に
  切り詰め、次の探索の最初の保存で持ち越しが全部消えないようにした。

English: fixes a visible-only solver abort (repetition window lost capacity on
move-assignment), VISIT_NOBLE depth labels in root split/frontier/proof DAG
(parallel sweeps reported mates one move short), restricted refutations
poisoning NativeMateSearchSession, and caps mate transposition tables at
DEFAULT_MAX_MEMO_STATES (2M) by default.

## 未出荷：探索木の辺を 16 バイトにし、スナップショットからまとめて符号化する（2026-11-07）

- V3 探索の辺（`Edge`）を 20 バイトから 16 バイトにした。決定節点だけが使うノイズ前の事前確率と、chance 節点だけが
  使う列挙値を同じ 4 バイトに重ねる。探索結果と `root_clean_priors()` はビット単位で同じ。
- `StateEncoder.encode_snapshots(data, offsets, players, ...)` を足した。スナップショットの列を `V3SearchSession` の葉と
  同じ並びの特徴量にまとめて符号化する（GIL を解放）。学習の replay buffer が特徴量ではなくスナップショットを持てる。
  V3 探索の葉の符号化も同じ関数（`StateEncoder::append_model_input`）を通る。

English: V3 search edges shrink from 20 to 16 bytes (the decision-only clean
prior and the chance-only enumerated value share one slot; results are
bit-identical). New `StateEncoder.encode_snapshots` batch-encodes game
snapshots in the V3 leaf layout, so a replay buffer can keep snapshots
instead of features.

## 未出荷：V3 探索の木の再利用で未到達の部分木を解放する（2026-10-31）

- `V3SearchSession.advance_game()`（`GameSearch::try_advance`）が再利用に成功したとき、新しい root から辿れる節点だけを
  新しい配列に幅優先順（root が 0）で詰め直し、`Edge::child` を付け替えて古い配列を解放する。これまでは root の添字を
  移すだけで、選ばれなかった手の部分木が 1 局の間ずっと残り、6,400 探索の自己対局でメモリが増え続けていた。
- 探索結果（root の訪問数・行動価値・事前確率・root 価値・乱数の消費）は詰め直す前（a00c5f2）とビット単位で同じ
  （`tests/data/v3_reuse_golden_a00c5f2.json`）。`node_count()` は再利用直後に新しい root の部分木の大きさになる。

English: tree reuse in the V3 search now compacts the node array to the new
root's subtree (breadth-first renumbering) and releases the rest; search
results are bit-identical to a00c5f2.

## 未出荷：レビューで見つかった6件の不具合の修正（2026-10-20）

- `V3SearchSession.apply()` は `offsets` が 0 から始まり、行ごとに増え（合法手0の行を拒否）、`legal_ids` の長さで
  終わることを、木に触れる前に確かめる（違反は `ValueError`）。不正な先頭値や合法手0の rollout 行が配列外の
  読み出しになっていた。C++ の `Session::apply` は
  合法手 ID の総数を受け取る。
- 空のバッチの `apply()` が `num_threads >= 2` で `ValueError: vector::reserve` になっていたのを直した（何もしない）。
- 木の探索予算をその回の `collect()` の中（終局・深さ打ち切りの逆伝播）で使い切った対局も、同じ `collect()` で
  rollout を始める。空のバッチで止める実行ループで rollout が抜け落ちていた。
- `near_mate_search`: `no_take_probe=True` で閾値を満たせなかった問い合わせは反証にしない（`verdict` は
  `inconclusive`、上限は下げず、詰みが無ければ `stop_reason` は `restricted_probe_exhausted`）。確率1の局面を
  「75%未満」と報告していた。
- 準詰み探索の攻め方の root も終局・深さの共通判定を通す。深さ 0 で勝率1、勝って終わった局面で勝率0を返していた。
- `root_mate_probe` と `MateSearchSession` で温めの時間上限 0（独自の上限なし）が下位の探索に「無制限」として渡り、
  全体の締め切りを越えていたのを直した。時間上限があれば常に締め切りの残りから正の時間を渡す。

English: fixes six review findings — offset validation in V3SearchSession.apply
(out-of-bounds read), empty-batch apply with threads, rollouts starting in the
collect that finishes the tree, restricted near-mate misses no longer lowering the
upper bound, terminal/depth rules at the attacker root of the near-mate search,
and an uncapped warm start no longer ignoring the shared deadline.

## 未出荷：V3 探索のサンプル型 chance 節点の漸進的拡幅（2026-10-20）

- `V3SearchConfig` に `chance_widening_base`（C）と `chance_widening_alpha`（α）を足した（既定はどちらも 0）。両方が正のとき、
  サンプル型 chance 節点が持つ結果の子を k(n) = ceil(C · n^α)（n は節点の訪問数、評価待ちの仮訪問を含む）に抑える。
  世界がめくる次のカードが既存の子でなく子の数が k(n) 以上なら、行動の前に `force_next_draw` で訪問数が最少の既存の子の
  カードを山札の先頭に置く。値の集計は変えない。列挙済みの chance 節点と `merge_own_deck_reveal` で分岐しない予約には効かない
  （`doc/mcts_v3.md`）。
- どちらかが 0 なら無効で、探索の出力は従来とビット単位で同じ（`tests/test_mcts_v3.py` が aaa9578 の出力を固定している）。
  負の値は `V3SearchSession` の構築で `ValueError`。`stats()` に強制回数 `chance_widenings` を足した。

English: V3 search gains progressive widening of sampled chance nodes
(`chance_widening_base`, `chance_widening_alpha`; both default 0 = off and
bit-identical to before). A chance node keeps at most ceil(C · n^α) revealed-card
children; a world about to reveal a card outside that set is forced
(`force_next_draw`) to the least-visited existing child before the action is
applied. Enumerated chance nodes and merged own-deck reservations are
unaffected; `stats()["chance_widenings"]` counts the forced reveals.

## 未出荷：購入済みカードを集合で持つ（2026-10-19）

- `PlayerState.purchased_cards` を購入順を持たない90ビットのカードIDの集合（`CardSet`、`src/card_set.h`）にした。
  `PlayerState` は152→80バイト、`Board` は592→448バイトになった。ルール・合法手・hashは変わらない。
- Python の `purchased_cards` はID昇順のlistを返す。代入は順不同を受け付け、範囲外のIDや重複は `ValueError`。
- snapshot の format・rules version は5のまま。購入済みカードはID昇順で書き出し、読み込みは並び順を問わない
  （購入順で書かれた過去の snapshot も読める）。同じ局面の snapshot のバイト列と、SPN の `bought:[…]` の並びは
  変わりうる。information state は元からID順なので変わらない。
- 準詰み探索の内部で使っていた `CardIdSet` は `CardSet` に統合した。
- 計測（300局面、同一マシン、学習の並走あり）: `clone_light` 約12.0ns→約9.6ns、複製＋`apply_trusted`
  約43.5ns→約38.6ns。

English: purchased cards are now an unordered 90-bit card-ID set. Python lists
them in ascending ID order, snapshots keep format/rules version 5 (written
ascending, read in any order), and Board shrinks from 592 to 448 bytes:
clone_light about 12.0 ns → 9.6 ns, clone + apply_trusted about 43.5 ns →
38.6 ns.

## 未出荷：局面の複製と手の適用の高速化（2026-10-18）

- `PlayerState` の購入履歴・取得貴族を固定長（90枚・12枚）にし、千日手の判定区間を8局面まで `Board` の中に持つ
  （`src/repetition_window.h`）。局面の複製は heap 確保を伴わなくなった。Python の `purchased_cards`・`acquired_nobles`
  に上限を超える長さを代入すると `ValueError` になる。
- 手の適用で千日手の局面キーを、トークンだけが動く決定のときだけ計算する。貴族の判定は資格のない手番で即座に終える。
- 計測（300局面、同一マシン）: `clone_light` 約28ns→約12ns、複製＋`apply_trusted` 約80ns→約41ns。ルール・合法手・
  hash・snapshot は変えていない。
- 使われなくなった性能カウンタ `purchased_card_vector_reallocations`・`acquired_noble_vector_reallocations` を削除した。

English: Board copies no longer allocate (fixed-capacity provenance lists and
an inline repetition window), and apply computes repetition keys only for
token decisions: clone_light about 28 ns → 12 ns, clone + apply_trusted about
80 ns → 41 ns. Rules and outputs are unchanged.

## 未出荷：場札予約の返却も補充を見てから選ぶ（行動空間V5）（2026-10-18）

- 場札の予約（`RESERVE_VISIBLE`）は返却を持たなくなった。予約（場札・山札）で11枚になったら返却フェーズ
  （`waiting_return`、`pending_decision=1`）に入り、同じプレイヤーが補充された場札・引いたカードを見てから
  `RETURN_GEM` で1枚返す。順序は「返却 → 貴族」。取得の返却は従来どおり同じActionに含む（`doc/engine_specs.md`）。
- 全行動policy用の行動空間を `ActionEncoderV5`（3049枠、`src/action_encoder_v5.h`）にした。場札予約は返却なしの
  12枠 `980 + level * 4 + slot`、購入は V3 id − 90。`ActionEncoderV4`（3121枠）と `v3_to_v4_id` /
  `v4_to_v3_id` / `v3_to_v4_table` は削除し、移行の手段は提供しない（`doc/action_space_v5.md`）。
  `v4_semantic_group_id` は `v5_semantic_group_id` に改名した（139群は同じ）。
- `V3SearchSession`、`near_mate_search` の `best_action_id`、`exact_mate_search` の `winning_root_action_id`、
  `root_mate_probe` の `action_id` は V5 ID になった。V2/V3 は返却フェーズを表現できず、返却付きの場札予約の枠は合法にならない。
- USI 1.3 / KIFU v1.2：正規形は `reserve:C10` → `return:U` の2手。旧来の1手表記 `reserve:C<id>/return:<C>` は
  入力に限り2手に展開する（`expand_usi_move`）。KIFU v1.0・v1.1 も読める。SPN は変えず、手番側の11枚は予約後の返却待ちとして読む。
- Game snapshot を format/rules version 5、情報集合を format 6 / rules 5 にした。version 1〜4 の snapshot は読めない
  （`Game.upgrade_snapshot_v1` は削除）。状態特徴量（196）と `pending_decision_features` の意味は変えない。
- 10枚での場札予約が補充ごとの返却判断になるため、詰み探索の節点数は増える（5手詰めのベンチ局面は約66万→150万）。
- `csplendor/api/usi_kifu.py` に残っていた、末尾の import で上書きされて使われない旧実装（USIの読み書き・棋譜の読み書き）を削除した。

English: Face-up reservations no longer carry a token return; any reservation
that reaches 11 tokens enters the return phase, and the return is chosen after
the refill is seen. `ActionEncoderV5` (3049 ids) replaces the deleted
`ActionEncoderV4`, with no migration helpers. USI 1.3 / KIFU v1.2 record the
return as a separate ply. Snapshots are version 5 only; information states are
format 6 / rules 5.

## 未出荷：準詰みの初手指定（2026-10-18）

- `near_mate_probability_cpp` に `required_root_action`（packed action code）を追加した。攻め側の初手をその手だけに
  限って準詰みの確率を評価する（`doc/mate_usage.md`）。dlsplendor の準詰みによる手の上書きが使う。

## 未出荷：V3 探索で観測者自身の山札予約を分岐させない選択肢（2026-10-17）

- `V3SearchConfig.merge_own_deck_reveal`（既定 `False`）を追加した。`True` では観測者の山札予約も chance 節点を作らず、
  その下の節点を共有する。相手の手の統計が「観測者が何を引いたか」を知る漏れが探索内で消える。chance 列挙はこの予約に
  効かず、この予約をまたぐ木の再利用はしない。root より前の伏せ札は既知の限界として `doc/mcts_v3.md` に書いた。

English: `merge_own_deck_reveal` keeps the observer's own deck reservation
inside the information set, so opponent nodes do not split by the drawn card.

## 未出荷：千日手を「同一局面4回目で引き分け」に改定（2026-10-14）

- 千日手の判定を、完全な局面（bank・場札・山札・貴族・両者のtoken／bonus／点数／予約／獲得貴族・手番・
  返却待ちなどの途中状態）が対局の中で4回目に現れたら引き分け（winner `-2`、`REPETITION`）に改めた。
  連続でなくてよい。旧規則（実質パスを両者3回ずつ連続）はこの規則に含まれ、同じ手で終局する。
  自己対局で、片方が組み合わせの違う返却を混ぜて旧判定をすり抜けた手詰まりの2局も千日手になる。
- `Board.passive_streak` を廃止し、判定区間 `Board.repetition_keys`（最後にカード・貴族・最終ラウンド・
  token総数が変わった後の局面の鍵）、`Board.repetition_count`、`Board.repetition_position_key()`、
  `Board.REPETITION_DRAW_COUNT`（4）を追加した。token総数は返却待ちを除いて減らないので、区間は厳密で、
  通常は空か短い。区間は厳密hash・観測hash（多重集合）・snapshot・情報集合に含み、探索の入力特徴量は変えない。
- MCTS・厳密な詰み探索・準詰み・ソルバーのoracle手は手の適用を共有するので、同じ規則で終局を判定する。
  10枚同士の交換が続く局面を区別するため、詰みのベンチ局面（5手詰め）のノード数は約62.5万から66万に増えた。
- Game snapshot を format/rules version 4、情報集合を format 5 / rules 4 にした。version 2・3 の snapshot は
  区間を空（その局面から開始）として読み込める。snapshot の上限を 64KiB に広げた。
- Web API の盤面の `passive_streak` を `repetition_count` に置き換えた。

English: A complete position occurring for the fourth time (not necessarily
consecutively) now draws by repetition, replacing the passive-streak rule.
`Board.passive_streak` is replaced by `Board.repetition_keys` /
`repetition_count`; snapshots are version 4 (2 and 3 still load) and
information states format 5 / rules 4.

## 未出荷：棋譜の結果に投了を追加（2026-10-11）

- 棋譜の結果に `Result: P<勝った側>_WIN RESIGN`（投了。投了したプレイヤーの相手の勝ち）を追加した（`doc/KIFU.md` §6）。
  盤面は終局しておらず、`FinalScores`・`TotalTurns` は投了の時点の値。読み書きは既存の detail と同じで、コードの変更はない。

## 未出荷：準詰みと厳密な詰みの深さ探索の C++ 化（2026-10-11）

- 準詰みの挟み込み（しきい値の問い合わせで深さごとに確率を挟む）を C++ の `near_mate_search`
  （`src/near_mate_probe.h`）にした。Python の `search_near_mate` はその薄いラッパーで、返す辞書の形式
  （`NEAR_MATE_FORMAT`）は変わらない。結果に最善手の V4 行動ID `best_action_id` を加えた。
- `search_reveal_verified_mate_depths(jobs=1)` の深さ探索を C++ の `exact_mate_search`
  （`src/exact_mate_search.h`）として公開した。`root_mate_probe` の終盤の厳密探索もこれを使う。
- どちらもシングルスレッドで Python に依存せず、WASM では `reveal_verified_solver.cpp` と
  `reveal_solver_components.cpp` だけでビルドできる。ノード上限の条件で旧実装と全項目一致をテストで確認した。

English: The near-mate bracketing (`near_mate_search`) and the jobs=1 exact
mate depth sweep (`exact_mate_search`) are now single-threaded C++ shared by
Python and WASM; `search_near_mate` keeps its dict format.

## 未出荷：千日手（2026-10-07）

- 両者が実質パス（`PASS`、または取ったのと同じ宝石を返す手 `Action.is_token_noop()`）を3回ずつ、
  連続6手番続けたら、千日手として引き分け（winner `-2`）で終局する。判定は手の適用の中で行うので、
  探索・詰みソルバー・Python・WASM が同じ終局を見る。先後を入れ替えた指し直しはしない。
- `Board.passive_streak`（連続回数）と `Board.end_reason`（`cs.GameEndReason`: `NONE` / `NORMAL` /
  `STALEMATE` / `REPETITION`）を追加した。回数は厳密hash・観測hash・snapshot・情報集合に含み、
  探索の入力特徴量（316次元）は変えない。
- Game snapshot を format/rules version 3、情報集合を format 4 / rules 3 にした。version 2 の snapshot は
  そのまま読める（回数0、終局済みなら `NORMAL`）。
- 棋譜の結果に `Result: DRAW STALEMATE`（両者パス）と `Result: DRAW REPETITION`（千日手）を追加した。
  Web API の盤面に `passive_streak`・`end_reason` を追加した。
- 詳細は `doc/engine_specs.md`「千日手」、`doc/KIFU.md` §6 を参照。

English: Six consecutive passive turns (three per player: PASS or a take
returning exactly the taken tokens) draw the game by repetition inside rule
application. New `Board.passive_streak` and `Board.end_reason`
(`GameEndReason`); snapshot v3 (v2 still loads), information state v4, KIFU
results `DRAW STALEMATE` / `DRAW REPETITION`. Search features are unchanged.

## 未出荷：root の詰みの探り（root_mate_probe、2026-10-05）

- AI が手を選ぶ前の root の詰み探索（発火条件、相手の非公開予約の除外、終盤予算、anytime の探り、
  終盤の厳密探索、合法手との照合）を C++ の `root_mate_probe`（`src/root_mate_probe.h`）にまとめ、
  `cs.root_mate_probe` / `cs.RootMateProbeConfig` / `cs.RootMateProbeResult` を追加した。
  スレッドと Python に依存しないので、WASM（embind）からも同じ結果で呼べる。
- ノード上限だけの条件では、dlsplendor の `RootMateSearch.probe`（新しい `MateSearchSession(jobs=1)`）と
  全項目が一致する。探りと厳密探索が1つの締め切りを共有するため、呼び出し全体が時間上限を超えない。
- 詳細は `doc/mate_usage.md` を参照。

English: New single-threaded `root_mate_probe` (C++, Python binding) reproducing
dlsplendor's RootMateSearch for both Python and WASM; one shared deadline
bounds the whole call.

## 未出荷：外部AI連携の削除（2026-10-05）

- Web APIの `POST /game/{session_id}/ai_move` と `GET /models`、`csplendor.api.ai_manager`・`ai_provider`・
  `external_ai_bridge`（`AIProvider`、`set_ai_provider()`、`CSPLENDOR_*_PATH` 環境変数）を削除した。
  AIは呼出し側で動かし、`action` / `action_usi` で着手する。PyTorchだけを入れる `[ml]` extraも削除した。
- 詰め問題集生成（`scripts/generate_mate_puzzles.py`）のGenbu/`dlsplendor` 対局を削除し、csplendorだけで動く
  `RandomPurchasePlayer`（購入手を優先する一様ランダム、`--seed` で再現可能）に置き換えた。`--genbu-*` 引数は廃止。
  進捗表示の段階名は `genbu_playout` から `playout` になった。
- csplendorは `dlsplendor` に依存しなくなった。

English: Removed the external-AI web endpoints (`/game/{id}/ai_move`, `/models`),
the `ai_manager`/`ai_provider`/`external_ai_bridge` modules and the `[ml]` extra.
Mate-puzzle generation no longer drives Genbu through `dlsplendor`; it plays
seeded random games that prefer purchases (`RandomPurchasePlayer`), and the
`--genbu-*` options are gone. csplendor no longer depends on `dlsplendor`.

## 未出荷：山札予約後の返却フェーズと行動空間V4（2026-10-05）

- 山札予約（`RESERVE_DECK`）は返却を持たなくなった。予約で11枚になると同じ手番のまま返却フェーズ
  （`Board.waiting_return`、`Board.pending_decision == 1`）に入り、めくれたカードを見てから
  `RETURN_GEM`（`ActionType` 7）で1枚返す。順序は「返却 → 貴族」。取得・公開予約の返却と購入は変更なし。
- `cs.ActionEncoderV4`（3121枠）を追加した。V3の山札予約21枠を3枠に畳み、`RETURN_GEM` 6枠を加えた。
  `v3_to_v4_table()` でV3のpolicyを移行できる。V2/V3は返却フェーズを表現できない（マスク空、`encode` は -1）。
  48枠の `ActionEncoderCpp` は返却フェーズ中だけslot 0..5を返却色に使う。
- `V3SearchSession` の行動IDをV4にした。`V3SearchConfig.pending_decision_features`（既定 True）で `waiting_return`・`waiting_noble` の2次元を末尾に足す（314→316次元）。
  `cs.v4_semantic_group_id` を追加した（139群）。
- Game snapshotをformat/rules version 2、情報集合をformat 3 / rules 2にした。version 1のsnapshotは
  拒否し、`Game.upgrade_snapshot_v1()` で一度だけ変換する。
- USIに `return:<C>` を追加した。山札予約の正規形は `reserve:L2` → `return:W` の2手。入力に限り旧表記
  `reserve:L2/return:W` を2手に展開する。棋譜の書き出しは `Splendor KIFU v1.1`（v1.0も読込可）。
  Web APIの盤面に `waiting_return`・`pending_decision` を追加した。
- 詰み探索では返却を予約手の一部として扱い、深さを消費しない。
- 詳細は `doc/action_space_v4.md` を参照。

English: Deck reservations no longer carry token returns; an 11-token
reservation enters a same-player return phase resolved by `RETURN_GEM` after
the drawn card is seen. New `ActionEncoderV4` (3121 ids) with a V3 migration
table; `V3SearchSession` now uses V4 ids and appends two pending-decision
features (waiting_return, waiting_noble) by default. Snapshot v2 (convert v1 with `Game.upgrade_snapshot_v1`),
information state v3, USI `return:<C>` and KIFU v1.1.

## 未出荷：ホットスポット高速化（2026-10-04）

- 探索結果・特徴量・行動IDを変えずに（mainとビット一致を確認）、エンジンのホットパスを高速化した。
  V3SearchSession は単一スレッドで main 比 1.31〜1.43 倍になった。
  内訳は、合法手の数え上げ、遅延MT19937、公開カード統計、V3コーデックの表引き、辺検索の改善である。
- `V3SearchConfig.num_threads`（既定1）を追加した。対局単位で collect/apply を並列化し、結果はスレッド数によらず同一である。
  64局ベンチでは、16スレッドで main（逐次）比 約8倍だった。
- `ActionEncoderV3.legal_action_ids(game)` を追加した。合法手のV3 IDを int32 配列で一括取得でき、Python で1手ずつ encode する場合の約20倍速い。
- スナップショット復元は約3.2倍、準詰みは約5%速くなった。
- 詳細・採用しなかった案・計測条件は `doc/speed_review_20261004.md` を参照。

English: Bit-identical hot-path optimizations (V3 search 1.31-1.43x single
threaded), an opt-in `V3SearchConfig.num_threads` for per-game parallel
collect/apply, and a batched `ActionEncoderV3.legal_action_ids`.

## 未出荷：最終高速化候補（2026-09-06）

- 計測・受入対象のエンジンは `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49`。
  F1記録は `49878b661298bf45e39c5f5ca5afa6d0e363736a`。本体・buildは不変。
  F2/F3記録 `e486e27cbabdec4387f9d183a758720e1cf2caee` 後、承認された5テストのimportだけ是正した。
- 採用済みexact hash、noble mask、合法手生成、solver sidecar/TT圧縮、score/scratch、
  visible rollback/take代表化、返却順位選択、legacy record統合、owning NumPyを保持。
  mainの情報集合v2・frontierヒント修正はF1で統合済み。新しい高速化は追加しない。
- portable nativeのmain対厳密めくれ探索は独立再測定2.373倍。
  Python特徴量12.808倍、特徴量＋step6.044倍は別workload。倍率の乗算・AI全体への外挿はしない。
- LTO既定OFF。追加効果と並列MCTSの累積高速化は未確定。4A保留・棄却案・reference/VERIFY/fallbackを保持。
- 実モデルselfplay12/selfplay17のCPU受入、GUIワーカーの5/7手frontier通信を確認。
  ブラウザ/GPU/旧Genbu/実モデル速度A/Bは未実施。
- **READY_FOR_REVIEW**：候補追加テスト5ファイルのCI lint違反7件を最小修正。
  CI lint/security lint、関連73件、Python 3.12全体595件・coverage 58.89%が通過。
  元の失敗証跡を保持。
- F4前半：作業branch push・PR #26作成、隔離clean wheel受入PASS。
  hosted CIはClang constexpr上限、Python 3.8 driver、Windows slot testsで失敗し **BLOCKED**。
  原因・最小修正案のみ記録。本体・build・既存testの追加修正は未実施。
  main統合・push、常用環境更新、Release/PyPI公開は未実行、merge・導入は承認待ち。

English: This is an **unreleased** candidate. Retained optimizations and F1
cumulative measurements are unchanged. Real CPU consumer smoke checks passed,
and the seven test lint failures are now fixed with import-only changes. Local
checks passed (595 tests, coverage 58.89%). F4 clean-wheel acceptance passed, but
PR #26 CI exposed Clang/Python 3.8/Windows compatibility failures: BLOCKED.
Only the candidate branch was pushed; merge/deployment remain unapproved. Native LTO remains
opt-in and OFF by default; no whole-AI speedup or completed depth-7 proof-time
claim is made.

根拠 / Evidence:
[F1](doc/performance_experiments/final_main_vs_candidate_20260906.md)、
[F2/F3](doc/performance_experiments/f2_f3_shipping_review_20260906.md)、
[F3是正](doc/performance_experiments/f3_lint_correction_20260906.md)、
[F4前半](doc/performance_experiments/f4_premerge_review_20260906.md)、
[F4準備](doc/performance_experiments/f4_integration_runbook_20260906.md)。
