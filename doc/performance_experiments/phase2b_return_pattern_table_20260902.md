# Phase 2B-H4a: token return patternのconstexpr表引き

測定日: 2026-09-02

関連成果物:

- `phase2b_return_pattern_table_20260902.csv`: 正式23 workloadと境界4 fixtureの集計
- `phase2b_return_pattern_table_evidence_20260902.json`: 全正式pair、固定slot、
  build identity、semantic recordを含むcompact evidence
- `raw/phase2b/phase2b-h4-return-formal-20260902.json.gz`: 正式raw artifact
- `raw/phase2b/phase2b-h4-return-extra-*-20260902.json.gz`: 境界fixture raw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H4a — token return patternのconstexpr表引き
Baseline commit: 693fb2e（H3棄却記録後、実装はH2状態）
Working commit: このreportを含むH4a commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: commit後にcleanを確認予定。元checkoutの未commit変更には触れず、独立worktreeで作業
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, H1/H2=ON,
             instrumentation=OFF, hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは`CSPLENDOR_RETURN_PATTERN_TABLE=OFF/ON`だけを変えた。22 pair / 11
crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot crossover。
両バイナリは1,049,392 bytes、GNU `size`のtextは894,530 bytes、`.text` SHA-256は
双方`3e702e5facffbe3657af3369b8b253283668c7220a7d7d36ceac535762c07304`。

## 24.2 仮説

返却数1〜3の6色compositionは6、21、56通りしかない。現行はbase actionごとに色ごとの
再帰を辿り、途中の`std::array`と最終`Action`をcopyしていた。全84パターンをcompile-timeに
従来と同じ辞書順で列挙し、所持数を超えるpatternだけfilterすれば、規則と順序を保ったまま
再帰・branch・中間copyを除去できる。

超過4以上のeditor局面は従来再帰へfallbackする。tableは矩形storageを含め約1.35 KiBの
read-only dataで、heap allocation・Board field・公開ABIは増やさない。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/move_generator.h` | `SMALL_RETURN_PATTERNS` | excess 1..3のconstexpr pattern表と件数static assertion | 低。全available/order oracleあり |
| `src/move_generator.h` | `emit_with_returns` | small excessを表filterでemit、4以上は従来再帰 | 低。既存sink/capを維持 |
| `CMakeLists.txt` | build option | H4a既定ON、code-identical A/B | 低。OFFで旧経路 |
| `scripts/benchmark_manifest.py` | build metadata | optionをallowlistしfingerprintから除外 | なし。計測補助のみ |
| `tests/rule_query_unit.cpp` | pattern order oracle | 4,096 available組×excess 3種で集合・順序を比較 | なし。testのみ |
| `tests/test_engine_benchmark_tools.py` | manifest contract | option記録とfingerprint同一性 | なし |

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h4-return-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h4-return-on --output-on-failure -j2` | 33/33 pass |
| pattern oracle | `rule_query_unit` | 12,288 available/excess状態で全pattern集合・順序一致 |
| reachable differential | `rule_query_unit` | 10,000局面以上、100,000合法手以上でcount/action/code/order/apply一致 |
| Python full | fresh Release extensionで`python -m pytest -q` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan | Debug、H4a=ON、hash verify=ON、`detect_leaks=0`でCTest | 33/33 pass、diagnostic 0 |
| TSan | Debug、H4a=ON、hash verify=ONでCTest | 33/33 pass、race/warning 0 |
| exact reveal | 正式A/B、`five_moves`, depth 5 | node/order/reveal/proof digest一致 |

editor fallbackは1.0072倍、5手かつ返却なしのfixtureは1.0022倍で中立だった。
V2/V3、forced pass、noble、`MAX_MOVES` capは全CTestをOFF/ON双方で通過した。

## 24.5 performance

Aは返却表OFF、BはON。rateはops/sまたはsim/s、RSSはrunner peakの中央値。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count / 250手 | 3.072 M/s | 3.063 M/s | 0.9979 | [0.9904, 1.0014] | 5,696 | 5,688 |
| legal codes / 250手 | 187,357/s | 337,821/s | **1.8142** | **[1.8006, 1.8233]** | 5,694 | 5,690 |
| legal actions / 250手 | 185,554/s | 350,856/s | **1.8575** | **[1.8390, 1.8794]** | 5,702 | 5,720 |
| random self-play apply | 1.872 M/s | 2.549 M/s | **1.3594** | **[1.3566, 1.3646]** | 5,726 | 5,732 |
| exact legacy 1T | 301,528/s | 300,403/s | 0.9875 | [0.9833, 0.9980] | 103,182 | 103,178 |
| shared tree | 3.498 M/s | 3.554 M/s | 1.0050 | [1.0026, 1.0135] | 5,696 | 5,692 |
| visible solver | 667,260/s | 688,653/s | 1.0301 | [1.0220, 1.0376] | 12,976 | 12,984 |
| exact reveal | 512,231/s | 508,790/s | 1.0073 | [0.9949, 1.0140] | 16,378 | 16,298 |

token-return境界ではcodes 1.9349倍、actions 2.0223倍。legacy MCTSは-1.25%で2% gate内、
他の主要workloadも中央値悪化2%以内である。RSS変化はprimary -4/+18 KiB、self-play +6 KiB。

Python境界の8独立process ABBA/BAAB交差測定は、C++ playout 1.1539倍、
`legal_actions` 1.0206倍、`legal_action_codes` 0.9946倍、`legal_action_count` 0.9818倍。
Python APIの退行上限2%を満たした。

hardware perf counter:

| metric | A | B | delta |
|---|---:|---:|---:|
| cycles | N/A | N/A | `perf_event_paranoid=4` |
| instructions | N/A | N/A | 同上 |
| IPC | N/A | N/A | 同上 |
| branch misses | N/A | N/A | 同上 |
| L1D misses | N/A | N/A | 同上 |
| LLC misses | N/A | N/A | 同上 |
| atomic RMW | unchanged | unchanged | 追加atomicなし |
| allocations | unchanged | unchanged | constexpr tableのみ |

## 24.6 semantic equality

```text
node count: exact reveal 139,868、A=B
legal moves: exact reveal 692,386、A=B
TT hits/stores: memo hits 17,163 / memoized states 67,615、A=B
action-order digest: legal actions/codes 9e5be642d05486c3、A=B
reveal-order digest: exact reveal digest 9dd9c11919a4581a、A=B
root visits: MCTS correctness counter一致
tree size: MCTS correctness counter一致
proof status: exact reveal status/reason/limit一致
```

全正式workloadと境界fixtureでdigest/correctness counterが一致した。

## 24.7 結論

```text
ACCEPT
```

legal codes +81.42%、legal actions +85.75%、self-play +35.94%で各gateを大幅に超え、
順序・cap・editor fallback・探索digestを維持した。約1.35 KiBのread-only tableに対して
効果が十分大きいため採用する。

残存リスクは、`MAX_MOVES`とは別にsmall pattern最大56をstatic storageへ固定していること。
件数static assertionとexhaustive order oracleで監視する。

commit hashは最終回答に記載する。次に実行すべき独立仮説は
**Phase 2B-H4b — purchase payment patternの表引きemit**のみとする。
