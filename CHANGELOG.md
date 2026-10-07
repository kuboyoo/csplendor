# 変更履歴 / Changelog

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
