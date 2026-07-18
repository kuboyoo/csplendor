# csplendor Architecture

`csplendor` はSplendorの状態遷移を担うルールエンジンです。

## 境界

- 入力: 現在局面、合法アクション、乱数シード。
- 出力: 次局面、合法手、勝敗、特徴量。
- 非責務: Web UI、ニューラルネット学習、モデル管理、BGA収集。

## 依存

- C++17
- pybind11
- Python packageとしての `csplendor`
- USI仕様は `usi` を参照する。
