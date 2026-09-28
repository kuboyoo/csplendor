# ローカル退避データ

削除予定の worktree に残っていた未追跡の検証記録を保管する場所です。
退避データは Git 管理対象外で、この README と `.gitignore` のみを管理します。
リモートへの push には退避データが含まれないため、復元にはこのローカルコピーが必要です。

## 2026-09-28 の退避

- コピー元: `../csplendor-final-candidate/doc/performance_experiments/f4_main_integration_20260906/`（リポジトリルート基準）
- コピー先: `doc/local_backups/f4_main_integration_20260906/`
- 対象: 66 ファイル（検証記録、ログ、補助スクリプト）
- 検証: コピー前後の相対パス一覧と全ファイルの SHA-256 が一致。
- コピー元は削除せず保持。

## mainを基点に文書を整理した際の退避

- `pre_main_docs_20260928/csplendor_codex56_engine_optimization_request.md`:
  切り替え前に未追跡だった依頼文書を保持。mainに同名の追跡ファイルがあるため、
  ローカル版を別名の退避ディレクトリへ移動し、SHA-256一致を確認。
