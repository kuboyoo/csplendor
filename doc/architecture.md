# csplendor アーキテクチャ

最終更新: 2026-08-05

`csplendor` は2人用 Splendor の状態、合法手、局面更新、探索、C++/Python
bindingを管理する。GUI、学習実験、モデル、収集データは対象外であり、USIの仕様は
`usi` repositoryを正本とする。

## レイヤーと依存方向

```text
FastAPI / Python利用者
        |
        v
Python application services / package helpers
        |
        v
pybind11 subsystem registrations
        |
        +--> MCTS / solver facades
        |          |
        |          v
        +--> encoding schema / encoders
        |          |
        |          v
        +--> rule query / transition / move generation
                   |
                   v
             domain state
```

下位層は上位のWeb、外部AI、Python application serviceへ依存しない。探索とsolverは
公開された`Game`/`Board`、合法手、packed action、hashを共有境界とし、互いの探索内部へ
依存しない。

## C++コア

- domain: `types.h`、`action.h`、`player.h`、`board.h`、`game.h`。`Game`が現在局面、
  mode、action/undo journalを所有する公開facadeである。
- rules: `rule_query.h`を副作用のない照会、`rule_transition.h`を内部更新、
  `move_generator.h`をallocation-freeな合法手列挙の正本とする。
- state safety: `state_invariants.*`がreachable/editor/search/serializedの4 profileを診断し、
  editor更新とtrusted hot pathは別のmutation gatewayを通る。
- encoding: `encoding_schema.h`がaction V1/V2/V3とstate feature V1のversion、size、offset、
  色順序を定義し、各encoderとPython helperが参照する。
- search: 逐次`MCTS`は公開の安定経路である。共有tree/root-parallel探索は
  experimental opt-inで、scheduler/session/treeの所有権を独立componentに閉じ込める。
- solver: visible-onlyとreveal-verifiedのfacadeはPImplを使い、共通value type、hidden outcome、
  proof DAG storageをcompiled coreへ分離する。

CMakeでは公開include/CPU設定を`csplendor_core_config`、非template実装を
`csplendor_core`に置く。Python module初期化は薄い`bindings.cpp`からdomain、rules、
encoding、MCTS、solver別の登録翻訳単位を呼ぶ。公開C++ headerの分類は
[`refactoring_contracts.json`](refactoring_contracts.json)を正本とし、public headerは
それぞれ単独includeでCI compileする。

## Python application層

FastAPI endpointは`GameSessionService`、`KifuApplicationService`、
`ReplayApplicationService`へ委譲する。process内状態は`SessionStore`/`KifuStore` protocolの
背後にあり、既定実装は単一process向け`InMemoryStore`である。JSONへの変換は
`game_presenter.py`が担う。

USI/KIFUはtoken/parser/DTO、legal-action resolver、serializer/codecに分かれ、仕様の正本は
`usi` repositoryとする。外部AIは`AIProvider`境界から遅延loadし、通常のengine importでは
sibling repositoryやmodel fileを探索しない。legacy replay pickleは管理者が配置した
信頼済みローカルファイル専用readerに隔離する。

## 状態・互換性

- 宝石IDは`Diamond, Sapphire, Emerald, Ruby, Onyx, Gold = 0..5`で固定する。
- 合法手は集合だけでなく順序と2048件超過時のprefixも互換契約とする。
- `clone()`はjournalを含むfull copy、`clone_light()`/`shuffled_clone*()`は現在局面だけの
  search copy、snapshotはversioned current stateである。
- 公開API、action ID、feature layout、snapshot、trace、proof digestを変える場合は
  リファクタリングと分離し、新versionと移行手順を用意する。

詳細は[`refactoring_contracts.md`](refactoring_contracts.md)を参照する。

## 検証境界

Python 3.8--3.12、native CTest、GCC/Clang strict warning、ASan/UBSan、TSan、package/wheel、
macOS arm64、Ubuntu arm64、Windows x64をCIで確認する。Python coverageは50% gate、
C++ coverageは現時点ではreport-onlyである。性能は固定hostの同一workload A/Bで評価し、
絶対値だけをmerge gateにはしない。
