# csplendor ドキュメント索引

最初に [README](../README.md) の必要環境・セットアップ・サンプルコードを参照してください。以下は利用目的別の詳細資料です。

## エンジン・データ仕様

| 資料 | 内容 |
|---|---|
| [エンジン仕様](engine_specs.md) | 2人用ルール、局面、手番、支払いモード |
| [カード・貴族カタログ](card_catalog.md) | カード90枚・貴族12種類のID、色、得点、コスト |
| [Python API](api_ref.md) | Game、Board、Action、静的データ |
| [snapshot](game_snapshot.md) | 完全情報を含む版付き局面保存 |
| [情報集合の識別](information_state.md) | 観測者別の局面キー・定石DB |
| [macOSビルド](building.md) | Apple Silicon、portable/native、wheel |

## AIを開発する

| 資料 | 内容 |
|---|---|
| [機械学習連携](ml_integration.md) | 196特徴、observer、行動マスク、schema |
| [AI向け仕様](ai_engine_spec.md) | 貴族選択・終局phase・Actionフィールド |
| [行動空間V2](action_space_v2.md) / [V3](action_space_v3.md) | 4869 / 3133枠の意味とエンコード |
| [詰み探索ガイド](mate_usage.md) | API、CLI、DAG、外部AIによる問題生成 |
| [ソルバー仕様](SOLVER.md) | 深さ・証明・Unknown・予算の契約 |
| [並列MCTS](parallel_mcts_usage.md) | サンプルと実験的APIの制約 |
| [V3多対局探索](mcts_v3.md) | 3,133行動上のPUCT。多対局の葉バッチ、決定化、サンプル型chance |
| [並列探索の実装状況](parallel_search_plan/implementation_status.md) | 対応済み機能・残る検証 |
| [速度ベンチマーク](performance_benchmarks.md) | 測定値、条件、対象commit、再現手順 |

## 対戦サービス・外部AIを接続する

| 資料 | 内容 |
|---|---|
| [READMEのサービス連携](../README.md#web-apiと対戦サービスへの組み込み) | 正規局面、秘匿情報、時間・排他・永続化の分担 |
| [Web API](web_api.md) | HTTPルート、支払いモード、状態レスポンス |
| [USI正本（別repo）](https://github.com/kuboyoo/usi/blob/main/docs/USI.md) | コマンド、SPN、着手記法の仕様 |
| [USI実装側資料](USI.md) | このrepoの互換性資料。仕様変更はusi側を先に更新 |
| [棋譜](KIFU.md) | 棋譜の保存・再生形式 |

## エンジン内部開発

- [アーキテクチャ](architecture.md): C++/Pythonの依存関係と責務。
- [互換契約](refactoring_contracts.md): API分類、状態不変条件、所有権。
- [技術概要](overview.md): コアの構造。

## 過去の検証・設計記録

以下は当時の対象commit・環境・判断を記録した資料です。現在の導入手順や最新HEADの測定値はREADMEとベンチマーク資料から確認してください。記録中の「承認待ち」「未完了」などは、その記録時点の状態です。

- [リリース検証](release_validation.md)
- [リファクタリング計画・完了記録](refactoring_plan_v2.md) / [フェーズ別記録](refactoring_plan/README.md)
- [MCTS高速化](mcts_hotpath_optimizations.md)
- [性能実験の記録ディレクトリ](performance_experiments/)
- [並列探索の設計・検証記録](parallel_search_plan/README.md)
