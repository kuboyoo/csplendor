# Phase 1B: observable Zobrist hashの差分維持

測定日: 2026-09-02

関連成果物:

- `phase1b_paired_20260902.csv`: 正式22-pairの集計
- `phase1b_paired_evidence_20260902.json`: build identity、全pair比、counter、
  correctness digest、作業時raw artifactのSHA-256
- `raw/phase1b/phase1b-*.json.gz`: 全pairを含む圧縮済み作業時原本

本Phaseの実装試作はcorrectnessを満たしたが、end-to-endの採用基準を満たさなかった。
そのため、コード、テスト、benchmark harnessの変更は全てrevertし、この報告とcompact
evidenceだけを残した。

## 24.1 状態

```text
Target phase: Phase 1B — observable Zobrist hash cache/差分維持
Baseline commit: c1a65a85cd519b63de0102e36712bbcc4b9e7cfe
Working commit: このreportを含むPhase 1B棄却記録commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: prototypeはrevert済み。commit後にcleanを確認
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, exact hash=ON,
             instrumentation=OFF, verify=OFF
CPU affinity: 1T=CPU 4、4T=CPU 4-7（set内にSMT siblingなし）
```

正式A/Bは同一sourceから
`CSPLENDOR_INCREMENTAL_OBSERVABLE_HASH=OFF/ON`だけを変えた。両buildの
fingerprintは
`55447a1b7ffd0cd8ab442e81503c6f239d0f204383eed15b29cfaae4c26ab988`
で一致した。22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、
ABBA、固定inode 2-slot crossoverで測定した。

A binaryは
`2bd853bcb9b099957d2539a63f22fbfcb3495f530b9d6da1c1806309edf241e2`、
B binaryは
`604e99685d1c2d2b7a220debe01ada07f301522e3c4949a2ce16750b4579a754`。
両方とも1,054,096 bytesで、`.text` SHA-256も
`95ba6b43a0db283c7436f4c38ae6d455d66048e611753f4b6e2d01d290afceaf`
に一致した。設定値1 byteとbuild-id以外の差を排除し、exact経路のcode-layout
差をA/Bへ混入させなかった。

環境はRyzen 9 7900X（12 core / 24 thread）、Linux
7.0.0-30-generic x86_64。CPU 4のgovernorは`performance`、boostは有効。
governorと`perf_event_paranoid`は変更していない。

## 24.2 仮説

Phase 0では`apply_only`が約20 M/s、`apply_observable_hash`が約16 M/sで、
determinized MCTSは遷移後に`observable_hash(observer)`を全再計算していた。
observable hashはdeck内容を含まないが、bank、visible、noble、両player、hidden
reserveの公開signature、deck size、turn/state flagsを毎回走査する。

Phase 1Aのtyped mutation primitiveで、固定observerのpublic identityをold/new saltの
XOR差分として維持すれば、この走査を除去できると予想した。自分のhidden reserve ID、
相手のslot/tier signature、deck size、turn、repetition意味論を維持し、determinization
前後で同observer cacheを引き継ぐ設計とした。

機序自体は成立した。8,192 simulationsのinstrumented legacyでは、
observable hash 17,096 callの全てが17,096 missから17,096 hitへ変わり、
1,179,624 field visitが0になった。一方、同じ探索で140,034 allocation /
28,245,023 bytes、8,192 Board snapshot copyは不変だった。Releaseのcold observable
hashは約38 M/s、すなわち1回約26 nsであり、探索全体ではtree管理とallocationの割合が
大きく、除去できる時間が採用想定より小さかった。

hardware cycles割合は取得不能だった。`perf_event_paranoid=4`のため
`perf stat`が`No supported events found`で終了した。権限設定は変更せず、
throughput、compile-time counter、allocation/RSSを根拠とした。

## 24.3 変更

以下は測定したprototypeの内容であり、最終commitには残していない。

| file | symbol | prototype change | compatibility risk |
|---|---|---|---|
| `CMakeLists.txt`, `src/hash_cache_config.*` | build/runtime gate | OFF/ONで本番`.text`を同一に保つimmutable gate | 低。prototypeはrevert |
| `src/board.h` | `cached_hash`、observer tag、`RuleMutator` | exactとobservableで1つのhash slotを共有し、既存paddingへobserver tagを格納。typed delta、uncached oracle、determinization preserveを追加 | 中。cache domain排他を広範囲に検証後revert |
| `src/game.h` | trusted apply dispatch | exact validを優先し、固定observer cache valid時だけobservable deltaを維持 | 中。着手規則本体は同一template |
| `src/state_invariants.cpp`, `src/state_field_roles.h`, `src/undo_record.h` | cache ownership | editor invalidation、invariant、snapshot/undo restoreへobserver tagを追加 | 低 |
| `src/perf_counters.*` | observable hit/miss | mechanism counterを追加 | 低 |
| `scripts/benchmark_engine_hotpaths.cpp` | observable workload/oracle | cold/cached micro、apply入力prime、両observer uncached oracleを追加 | 低 |
| `scripts/benchmark_manifest.py`, `scripts/run_paired_benchmarks.py` | A/B contract | Phase 1B軸とhit/miss measurementを追加 | 低 |
| `tests/*hash*`, `tests/state_copy_unit.cpp`, `tests/undo_record_probe.cpp` | differential tests | 全ActionType、observer、hidden情報、clone/undo/editor/failureを追加 | なし |

最終commitの変更は次の3つだけである。

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `doc/performance_experiments/phase1b_observable_hash_20260902.md` | report | 棄却理由と§24証跡 | なし |
| `doc/performance_experiments/phase1b_paired_20260902.csv` | summary | 正式paired集計 | なし |
| `doc/performance_experiments/phase1b_paired_evidence_20260902.json` | compact evidence | environment、counter、digest、raw SHA | なし |

prototypeでも`sizeof(Board)=392`、`sizeof(Game)=448`、layout digest
`210248ca4912bb94`がOFF/ON/Phase 1Aで一致した。したがってBoard copy回帰はfield
肥大化ではなく測定ノイズだけであり、sidecarへ移す条件は発生しなかった。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase1b-release-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase1b-release-on --output-on-failure -j2` | 33/33 pass |
| verify build | Debug、observable=ON、verify=ON、instrumentation=ONでCTest | 33/33 pass、oracle failure 0 |
| incremental differential | `incremental_hash_unit` | 1,000 games、全検査pass |
| separate-process differential | `validate_engine_differential.py --seeds 1000 --fixture-plies 32 ...` | 1,000 states / 12,000 semantic records一致 |
| benchmark contract | `python -m pytest tests/test_engine_benchmark_tools.py -q` | 23 pass |
| ASan+UBSan | Debug、verify=ON、`ASAN_OPTIONS=detect_leaks=0 ctest ...` | 33/33 pass、ASan/UBSan diagnostic 0 |
| LeakSanitizer | 通常ASan CTest | executorがptrace下のためLSan自体が起動拒否。上記再実行でleak検出のみ無効化 |
| TSan / Python full | 未実施 | end-to-end性能gate不通過でprototypeを棄却したため、採用gate前に停止 |
| reverted tree | clean Release rebuildのCTest、benchmark tool pytest、`py_compile` | 33/33 pass、22 pass、pass |

`incremental_hash_unit`の内訳:

- 1,000終局、57,440 trajectory transitions
- 1,941 all-legal states、50,087 successors
- exact oracle 441,724回
- observable oracle 1,075,980回
- observable valid/invalid shadow transition 各215,088回
- 全7 ActionType、observer 0/1
- hidden reserve ID/tier/flag、deck size、reserve/noble slot shift、turn
- exact/observable shared cache domain、observer 2/255 fallback
- editor、failed apply、clone、snapshot、undo、repetition、determinization 256 cases

separate-process corpusはsetup action 31,998、legal action 23,617、corpus digest
`c023bc2b535b0a15f61cea65cc7df2890b48bbcb4e8b14b793f2567be5b6935a`
で一致した。semantic failure、observable/exact oracle failureはいずれも0。

## 24.5 performance

Aはprototype cache OFF、BはON。rateはops/sまたはsim/s、RSSはnative processの
中央値（guard一括測定だけrunner RSS）。全行22 pair、95% CIは11 crossover blockの
bootstrap。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| determinized legacy / midgame | 266,908 | 269,790 | **1.0148** | [0.9963, 1.0227] | 109,024 | 109,010 |
| determinized legacy / hidden reserve | 264,467 | 264,230 | **0.9969** | [0.9917, 1.0074] | 113,164 | 113,146 |
| determinized shared sharded 1T | 274,039 | 276,829 | **1.0133** | [1.0063, 1.0181] | 50,228 | 50,228 |
| determinized root-parallel 4T | 862,340 | 865,264 | **1.0054** | [0.9935, 1.0163] | 72,760 | 72,724 |
| cached observable hash | 38.997 M | 166.151 M | 4.2662 | [4.2522, 4.3104] | 5,750 | 5,724 |
| cold observable hash | 38.062 M | 38.105 M | 1.0010 | [0.9959, 1.0132] | 5,742 | 5,716 |
| apply + observable hash | 15.314 M | 16.958 M | 1.1098 | [1.0976, 1.1281] | 7,724 | 7,738 |
| Board copy/restore | 12.736 M | 12.800 M | 1.0045 | [0.9995, 1.0202] | 5,628 | 5,576 |
| apply + exact hash | 16.733 M | 16.702 M | 0.9985 | [0.9960, 0.9991] | 7,732 | 7,736 |
| Phase 1A vs prototype exact legacy | 305,792 | 305,208 | 1.0073 | [0.9927, 1.0187] | 101,660 | 101,676 |

主対象は全て3%未満だった。shared 1TだけはCI下限が1.0を超えたが改善は+1.33%、
legacy midgameとroot-parallelはCIが1.0を跨ぎ、hidden-reserveは中央値-0.31%だった。
単一fixtureだけの改善でもなく、legacy/root/sharedの主経路で再現する3%改善でもない。

非対象guardは次の範囲だった。

| guard | B/A | 95% CI |
|---|---:|---:|
| legal count | 1.0048 | [0.9996, 1.0131] |
| legal codes | 0.9998 | [0.9958, 1.0015] |
| random self-play apply | 0.9993 | [0.9970, 1.0022] |
| apply only | 0.9989 | [0.9678, 1.0039] |
| clone light | 1.0114 | [0.9974, 1.0453] |
| determinization clone | 1.0016 | [0.9980, 1.0075] |

OFF/ON prototypeの`.text`は完全一致しており、exact pathの実行codeも同一である。
Phase 1A実バイナリ対prototype ONのexact legacyも中央値+0.73%、CI
[-0.73%, +1.87%]で、1%を超える確定回帰は認めなかった。ただしこのguardを満たしても、
主対象3%未達を覆さない。

mechanism counter:

| counter | A | B | delta |
|---|---:|---:|---:|
| observable hash calls | 17,096 | 17,096 | 0 |
| observable cache hits | 0 | 17,096 | +17,096 |
| observable cache misses | 17,096 | 0 | -17,096 |
| observable fields visited | 1,179,624 | 0 | -100% |
| Board snapshot copies | 8,192 | 8,192 | 0 |
| allocations | 140,034 / 28,245,023 bytes | 同左 | 0 |
| hash oracle failures | 0 | 0 | 0 |

hardware perf counter:

| metric | A | B | delta |
|---|---:|---:|---:|
| cycles | N/A | N/A | `perf_event_paranoid=4` |
| instructions | N/A | N/A | 同上 |
| IPC | N/A | N/A | 同上 |
| branch misses | N/A | N/A | 同上 |
| L1D misses | N/A | N/A | 同上 |
| LLC misses | N/A | N/A | 同上 |
| atomic RMW | unchanged | unchanged | cache deltaにatomicなし |
| allocations | 140,034 | 140,034 | 0 |

## 24.6 semantic equality

```text
node count: legacy evaluated boards 65,536、shared completed 32,768、
            root-parallel completed 49,152、全pair A=B
legal moves: midgame ordered legal count 250、hidden-reserve 30、A=B
TT hits/stores: N/A（solver非対象）
action-order digest: midgame f05af3b244e08b82、
                     hidden-reserve 0272d7bb8b5da361、A=B
reveal-order digest: N/A（solver非対象。observable tree/replay digestを比較）
root visits: legacy 65,520、shared 32,768、root-parallel 49,152、A=B
tree size: legacy 65,521、shared 32,764、root-parallel 49,155、A=B
proof status: N/A（solver非対象）
```

legacy midgameのformal traceはexpanded key
`f543d6d3479be346`、selected action `806c5b27561bdc7c`、inference request
`a844671db9af5331`、replay `5bf5b63bca21ef9e`で一致した。

sharedはroot visit `1cb35e6d9171153a`、root Q
`74606d29cb4d7f03`、expanded key `bd07ace533a27832`、selected action
`7e0e77f9c151f646`で一致した。root-parallelはroot visit
`870ff5c8ba3e449c`、root Q `26119920b9c9abc3`、worker result
`ade9b41440fa57f5`、selected action code `1049084`で一致した。

## 24.7 結論

```text
REJECT_AND_REVERT
```

observable hashの差分維持は、cache hit 100%化、field visit 100%削減、cached micro
4.27倍、apply+observable +10.98%という局所効果を確認した。しかし代表的な
determinized legacy/root/sharedの改善は-0.31%〜+1.48%に留まり、Phase 1B固有の
「主経路3%以上」と共通の「複数fixtureで再現」の採用基準を満たさなかった。
複雑なcache domain管理を本体へ残す費用に見合わないため、prototypeを完全にrevertした。

残存リスクは、将来tree/allocation costが別Phaseで下がった場合や、1 simulationあたりの
observable hash callが多い別workloadでは、相対効果が3%を超える可能性があること。
その場合も本実装をそのまま復活させず、新しいprofileに基づいてsidecar/child-key案を
再評価すべきである。

commit hashは最終回答に記載する。次に実行すべきPhaseは
**Phase 2A — rule transitionの小規模高速化**のみとする。
