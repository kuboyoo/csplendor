# Phase 2B-H5: 内部packed code sink

測定日: 2026-09-02

関連成果物:

- `phase2b_packed_code_sink_20260902.csv`: 正式23 workloadと境界4 fixtureの集計
- `phase2b_packed_code_sink_evidence_20260902.json`: 全正式pair、固定slot、
  build identity、semantic recordを含むcompact evidence
- `/tmp/phase2b-h5-packed-formal-final-20260902.json`: 正式raw artifact
- `/tmp/phase2b-h5-packed-final-extra-*-20260902.json`: 境界fixture raw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H5 — 内部packed code sink
Baseline commit: e57bd79（H4b棄却記録後、実装はH4a採用状態）
Working commit: このreportを含むH5 commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, H1/H2/H4a=ON,
             instrumentation=OFF, hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは`CSPLENDOR_PACKED_CODE_SINK=OFF/ON`だけを変えた。22 pair / 11
crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot crossover。
両バイナリは1,054,720 bytes、GNU `size`のtextは894,742 bytes、`.text` SHA-256は
双方`950647fd3d85c931055a2c02e78e84efbb147cc0f0e18b37557d24a51e58c9e6`。

## 24.2 仮説

`legal_action_codes()`は内部でwide `Action`を最終手ごとに作り、その直後にpackしていた。
特に返却付き手は同じbase actionを多数copyし、各回6色をpackする。base codeを一度だけpackし、
返却patternの4 bit laneを直接ORすれば、公開`Action` APIと規則列挙順を変えずにcopyとpackを
削減できる。

規則・cap・強制PASS・貴族・支払い・返却filterは
`consume_all_capped_impl<PackedCodes>`と`emit_with_returns_impl<PackedCodes>`の単一実装を
compile-time特殊化する。`legal_actions`はAction sink、`legal_action_count`は既存count sink、
`legal_action_codes`と`legal_action_code_at`だけがcode sinkを選ぶ。

最初の一律code sinkは250手で改善した一方、予約枠が満杯の5手fixtureで1.9%悪化した。
予約3枠が埋まると最大15個のreserve base actionが消えるため、その低可動性分類だけ旧Action
sinkへ戻す1比較のadaptive policyを追加した。最終5手結果は0.9978倍で中立となった。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/action.h` | `Action::pack_return_gems` | base codeへ6色返却laneを直接合成 | 低。16,384 case oracleあり |
| `src/move_generator.h` | `consume_all_capped_impl` | Action/codeを同じcap・forced-passロジックからemit | 低。compile-time特殊化 |
| `src/move_generator.h` | `emit_with_returns_impl` | base codeを一度packし、返却variantを直接code化 | 低。filter/order共通 |
| `src/game.h` | code APIs | fixed scratchへcode sinkで単回列挙、予約枠満杯時は旧sink | 低。公開型・順序不変 |
| `CMakeLists.txt`, benchmark manifest | build option | H5既定ON、code-identical A/B | 低。OFFで旧sink |
| `tests/rule_query_unit.cpp` | packed return oracle | 4 action type×4^6返却値を従来`Action::pack`と比較 | なし。testのみ |
| `tests/test_engine_benchmark_tools.py` | manifest contract | option記録とfingerprint同一性 | なし |

heap storage、Board layout、公開binding、Action wire formatは変更しない。購入候補そのものは従来の
Action再帰を共有し、code sinkは最終codeへの変換だけを専用化する。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h5-packed-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h5-packed-on --output-on-failure -j2` | 33/33 pass |
| packed return oracle | `rule_query_unit` | 16,384 helper case一致 |
| reachable differential | `rule_query_unit` | 10,000局面以上、100,000合法手以上でcount/action/code/order/apply一致 |
| Python full | fresh Release extensionで全`tests/` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan | Debug、H5=ON、hash verify=ON、`detect_leaks=0`でCTest | 33/33 pass、diagnostic 0 |
| TSan | Debug、H5=ON、hash verify=ONでCTest | 33/33 pass、race/warning 0 |
| exact reveal | 正式A/B、`five_moves`, depth 5 | node/order/reveal/proof digest一致 |

強制PASS、waiting noble、V2/V3、`MAX_MOVES` cap、editorのexcess>3 fallbackはRelease・
sanitizer・reachable differentialで通過した。全正式workloadと境界fixtureでdigestおよび
correctness counterが一致した。

## 24.5 performance

Aはpacked sink OFF、BはON。rateはops/sまたはsim/s、RSSはrunner peakの中央値。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count / 250手 | 2.966 M/s | 2.975 M/s | 1.0056 | [0.9942, 1.0112] | 5,700 | 5,684 |
| legal codes / 250手 | 333,029/s | 377,025/s | **1.1321** | **[1.1290, 1.1364]** | 5,694 | 5,696 |
| legal actions / 250手 | 338,027/s | 337,653/s | 0.9919 | [0.9800, 1.0197] | 5,684 | 5,702 |
| random self-play apply | 2.535 M/s | 2.517 M/s | 0.9930 | [0.9890, 0.9970] | 5,704 | 5,684 |
| exact legacy 1T | 281,642/s | 286,068/s | 1.0134 | [0.9981, 1.0249] | 103,166 | 103,164 |
| shared tree | 3.367 M/s | 3.530 M/s | 1.0340 | [1.0094, 1.0477] | 5,692 | 5,690 |
| visible solver | 688,002/s | 688,509/s | 1.0018 | [0.9971, 1.0083] | 13,022 | 13,040 |
| exact reveal | 516,600/s | 517,347/s | 1.0018 | [0.9931, 1.0042] | 16,280 | 16,304 |

境界`legal_codes`はtoken return 1.1127倍、gold payment 1.0627倍、editor fallback
1.4450倍。adaptive対象の5手局面は0.9978倍、CI [0.9838, 1.0054]で中立だった。
主対象3% gateを大きく上回り、他の主要workloadの中央値悪化は2%以内、RSS差も小さい。

fresh Python extensionの4 process×各5 sample、ABBA/BAAB交差測定では、
`legal_action_codes` 0.9888倍 [0.9706, 1.0085]、`legal_actions` 1.0087倍
[0.9995, 1.0166]、count 0.9946倍、C++ playout 0.9864倍で、確定退行はなかった。
このPython fixtureは予約枠満杯となりadaptive旧sinkを選ぶため、native 250手fixtureの改善を
反映しない。

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
| allocations | unchanged | unchanged | fixed scratch/vectorの既存1 allocationのみ |

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

## 24.7 結論

```text
ACCEPT
```

公開API・規則・順序を変えずに、主要な合法手code生成を13.21%高速化した。低可動性局面は
adaptive fallbackで中立化でき、全回帰・sanitizer・semantic gateを満たしたため既定ONで
採用する。これでPhase 2BのH1〜H5評価を完了し、次はPhase 3Aへ進む。
