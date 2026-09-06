# 変更履歴 / Changelog

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
  元の失敗証跡を保持。リモートCI未実行、F4は未実行・承認待ち。
  main統合、push、PR作成、常用環境更新、Release/PyPI公開は未実行。

English: This is an **unreleased** candidate. Retained optimizations and F1
cumulative measurements are unchanged. Real CPU consumer smoke checks passed,
and the seven test lint failures are now fixed with import-only changes. Local
checks passed (595 tests, coverage 58.89%); remote CI and F4 remain pending.
Status: READY_FOR_REVIEW, not shipment approval. Native LTO remains
opt-in and OFF by default; no whole-AI speedup or completed depth-7 proof-time
claim is made.

根拠 / Evidence:
[F1](doc/performance_experiments/final_main_vs_candidate_20260906.md)、
[F2/F3](doc/performance_experiments/f2_f3_shipping_review_20260906.md)、
[F3是正](doc/performance_experiments/f3_lint_correction_20260906.md)、
[F4準備](doc/performance_experiments/f4_integration_runbook_20260906.md)。
