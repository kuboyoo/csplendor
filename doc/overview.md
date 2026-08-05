# csplendor 概要

最終更新: 2026-08-05

`csplendor` は、2人用 Splendor のルール実行と木探索に向けたC++17エンジンである。
pybind11によるPython API、固定長action space、state feature、逐次/実験的並列MCTS、
検証solver、FastAPI連携を提供する。

## 設計上の重点

- 高速性: 合法手生成、局面copy、action mask、tree edgeをMCTS hot path向けに最適化する。
- 互換性: 合法手順序、packed action、encoder、feature、snapshot、USI変換をversion付き契約で守る。
- 再現性: seed、portable RNG、deterministic trace/replay、固定corpusで差分を検証する。
- 境界: ルールエンジンは外部AI、学習処理、モデル、GUIを所有しない。
- 可搬性: portable buildを配布の既定とし、Apple Silicon native buildはローカル専用とする。

## 主な機能

- `Game`/`Board`/`PlayerState`による局面管理、apply/undo、versioned snapshot。
- allocation-free emitterによる合法手の列挙、count、packed code、index適用。
- 48手のV1、4869手のV2、3133手のV3 action encodingと196要素のstate feature V1。
- hidden informationのobserver-aware determinization。
- 安定した逐次MCTSと、experimental opt-inのshared-tree/root-parallel MCTS。
- visible-only/reveal-verified solverとPython DFPN/puzzle tooling。
- session、KIFU、replayをservice/store境界へ分けたoptional FastAPI application。

## Repository構成

- `src/`: C++ domain、rules、encoding、MCTS、solver、binding。
- `csplendor/`: Python公開API、encoding補助、optional Web application。
- `tests/`: Python契約/property/differential/performance testとnative subsystem test。
- `scripts/`: benchmark、再現、solver/puzzle CLI。
- `doc/`: 内部仕様、検証記録、計画。USI仕様そのものは`usi`側を正とする。

Pythonは3.8以上、CMakeは3.13以上、C++17 compilerを前提とする。導入・性能値は
repository rootの`README.md`、依存方向は[`architecture.md`](architecture.md)、ゲーム契約は
[`engine_specs.md`](engine_specs.md)を参照する。
