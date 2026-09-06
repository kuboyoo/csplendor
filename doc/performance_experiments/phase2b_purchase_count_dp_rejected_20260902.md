# Phase 2B-H3: purchase payment count専用DP（棄却）

測定日: 2026-09-02

関連成果物:

- `phase2b_purchase_count_dp_rejected_20260902.csv`: 正式4 workloadの22-pair集計
- `phase2b_purchase_count_dp_rejected_evidence_20260902.json`: 全pair、固定slot、
  build identity、semantic recordを含むcompact evidence
- `raw/phase2b/phase2b-h3-formal-rejected-*-20260902.json.gz`: 作業時raw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H3 — purchase payment count専用DP
Baseline commit: c171cf3（Phase 2B-H2採用後）
Working commit: 実装はrevertし、この棄却記録のみをcommit
Branch: perf/codex56-engine-hotpaths
Working tree status: H3実装revert後、文書だけをcommit予定
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3
Build flags: Release, -O3 -DNDEBUG, C++17, portable, H1/H2=ON
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは`CSPLENDOR_PURCHASE_COUNT_DP=OFF/ON`だけを変え、両方の`.text`
SHA-256を
`c7678547398d8120b135d3e9a936b93ea61c9ef2b615aa02afbb91fd13893a2e`
へ一致させた。22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、
ABBA、固定inode 2-slot crossoverである。

## 24.2 仮説

count経路でも購入可能な各gold割当をwide `Action`として再帰生成していたため、5色×gold使用量の
bounded DPで件数だけを求めればAction構築を除去できると予想した。

最初の2-array DPは全36 laneの初期化・copyが支配し、250手legal count 0.739倍、
gold-payment 0.592倍だった。次にbounded interval convolutionをsliding window化し、
実際のgold幅だけを未初期化scratch上で更新した。以下はこの最適化版の正式結果である。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/move_generator.h` | experimental payment DP | purchase emitをcount-only sliding-window DPへ置換 | 中。count/emit規則の二経路化 |
| `CMakeLists.txt`, benchmark manifest | experiment toggle | code-identical OFF/ON軸 | なし。実験用 |
| `tests/rule_query_unit.cpp` | payment oracle | bounded interval 600,000状態、全90 card補助corpus | なし。実験用test |

上記コード・option・testは採用基準未達のため全てrevertした。commitへ残す変更は本報告、CSV、
compact evidenceだけである。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h3-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h3-on --output-on-failure -j2` | 33/33 pass |
| payment DP oracle | `rule_query_unit` | 600,000 interval状態 + 全90 card×64状態、cap含め一致 |
| reachable differential | `rule_query_unit` | 10,000局面以上でcount/actions/codes/order/apply一致 |
| semantic digests | 正式4 workload | 全pairで一致 |

性能gateで明確に棄却されたため、実験候補に追加sanitizer/Python fullは実行していない。
revert先のH2採用状態はASan+UBSan、TSan、Python fullを既に通過済みである。

## 24.5 performance

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count / 250手 / full | 3.043 M/s | 2.945 M/s | **0.9648** | **[0.9612, 0.9688]** | 5,674 | 5,674 |
| legal count / gold / full | 2.792 M/s | 2.518 M/s | **0.8989** | **[0.8943, 0.9059]** | 5,664 | 5,664 |
| legal count / gold / simple | 2.662 M/s | 3.085 M/s | 1.1545 | [1.1487, 1.1614] | 5,664 | 5,664 |
| random self-play / full | 1.848 M/s | 1.809 M/s | **0.9825** | **[0.9710, 0.9901]** | 5,668 | 5,664 |

simple modeは「最小gold割当1通り」を即答できるため改善したが、既定のfull modeは主要局面
-3.52%、gold-rich局面-10.11%、self-play-1.75%。主対象10%改善gateを逆方向に外した。

hardware perf counterは`perf_event_paranoid=4`のため取得不能。heap allocationは増えないが、
候補は呼出しごとに2×36×4 bytesのstack scratchを使用した。

## 24.6 semantic equality

```text
node count: 対象外（rule generation micro）
legal moves: 全正式pairで一致
TT hits/stores: 対象外
action-order digest: 6d9802c2ed236f83 / 9c7be20f4b104783、A=B
reveal-order digest: 対象外
root visits: self-playは固定seed/action digest一致
tree size: 対象外
proof status: 対象外
```

## 24.7 結論

```text
REJECT_AND_REVERT
```

sliding-windowまで最適化してもfull paymentの再帰emitより遅く、self-playもCI全体が悪化側だった。
simple modeだけの改善を残す複雑性にも見合わないため、実装を全て戻した。残存コードリスクはない。

この文書を含むcommit hashは最終回答に記載する。次に実行すべき独立仮説は
**Phase 2B-H4 — return/payment patternの表引きemit**のみとする。
