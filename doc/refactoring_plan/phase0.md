# Phase 0 実装記録

`README.md` の Phase 0 で要求する「後続のリファクタリングを安全に
比較するための観測点」を追加した。hash contractとMCTSの情報状態選択を
correctness修正として変更し、ゲームルールと合法手生成は変更していない。

## 追加物

- `tests/test_engine_baseline_contracts.py`
  - 公開 `__all__`、encoder の mask shape/dtype/返却後の独立性、固定seedの
    合法手code順序、code/index/applyの同値性を固定する。
  - `PlayerState` の通常setterと `Board.set_player` が packed fieldを同期する
    ことを確認する。
  - hash のhot-cache、完全局面hash、公開情報hashの定義域を確認する。公開情報
    hashは得点・count・final/winner・turn・正確なdeck枚数を含み、deck順序は含まない。
  - MCTSのexpand/select/backprop/virtual-lossと、Python callbackへ渡るGameが
    historyを含む値copyで、rootから隔離される現挙動を固定する。
  - `MCTSConfig.use_determinization` の既定値を有効化し、実戦MCTSが公開情報
    hashとhidden informationのdeterminizationを使うことを固定する。
- `scripts/phase0_native_probe.py`
  - 一時ディレクトリでのみC++ probeをbuildし、`sizeof`、full/light cloneの
    allocation数、実際に保持されたhistory長をJSONで出力する。
  - 一時的にinstrumentした `MoveList::push_back` で、2048件以降のemit試行を
    数える。`src/` と拡張moduleは変更しない。
- `scripts/benchmark_phase0.py`
  - fixed corpusのraw rate samplesをJSONに保存し、baseline/candidateを
    paired bootstrap CIで比較する。raw artifactはCI artifactまたは外部保存先に
    置き、repoには入れない。
- `tests/test_benchmark_contracts.py`
  - 比較統計が決定的に動作し、明白な回帰を検出することを確認する。

## 実行方法

```bash
python -m pytest -o addopts= tests/test_engine_baseline_contracts.py tests/test_benchmark_contracts.py -q
python scripts/phase0_native_probe.py --output /tmp/csplendor-phase0-native.json
python scripts/benchmark_phase0.py --label baseline --samples 15 --output /tmp/baseline.json
python scripts/benchmark_phase0.py --label candidate --samples 15 --output /tmp/candidate.json
python scripts/benchmark_phase0.py --compare /tmp/baseline.json /tmp/candidate.json
```

比較結果の `candidate_over_baseline_ci95` の上限が `1.0` 未満なら、統計的な
揺らぎを超える性能低下として変更を確定しない。比較前にPython、拡張module、
compiler、CPU条件、固定corpusが同一であることもJSON metadataで確認する。

## 今回の記録

- native probe（GCC、random reachable corpus 128局）: 最大retained actionは596、
  overflow attemptは0。これは公式到達局面を網羅した容量証明ではなく、editorが
  受理する非canonical stateも含まない。
- layout: `Action=21`, `MoveList=43010`, `PlayerState=104`, `Board=392`,
  `Game=448`, `MCTSNode=832` bytes。
- copy probe: retained history 50ではfull clone 39 allocations/light clone 2、
  retained history 84ではfull clone 124/light clone 3だった。global `new` counter
  はこのcompiler/allocatorでの診断値であり、絶対値をportableな性能契約にはしない。
- paired runner: 15 raw samplesを `/tmp/csplendor-phase0-benchmark.json` に採取した。
  同一artifactとのself comparisonは全metricでCI `[1.0, 1.0, 1.0]` となった。

## 残存する既知事項
- `tests/test_api.py` はこの環境で最初の `TestClient` requestが停止する。
  installed `httpx==0.28.1` はprojectのdev指定 `httpx<0.28` の外である。依存を
  指定範囲へ揃えた環境で、APIを含む全testを再実行する必要がある。
- overflow probeはrandom reachable corpusだけである。capacityを縮小・固定する
  判断は、reachable corpus、editor境界state、または上限証明が揃うまで行わない。
