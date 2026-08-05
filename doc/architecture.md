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

## Python application層

FastAPI endpointは`GameSessionService` / `KifuApplicationService`へ委譲し、
process内sessionは`SessionStore` / `KifuStore` protocolの背後に置く。現在の既定実装は
単一process向け`InMemoryStore`である。response schema変換は`game_presenter.py`が担う。

USI/KIFUはtoken/parser/DTO/legal resolver/serializer/codec surfaceへ分割し、仕様の正本は
引き続き`usi` repositoryとする。外部AIは`AIProvider`だけをapplication境界とし、通常の
engine importはmodelや外部repositoryを探索しない。legacy pickle replayはtrusted local
file専用readerに隔離する。
