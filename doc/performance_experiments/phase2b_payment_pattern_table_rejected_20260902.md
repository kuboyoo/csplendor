# Phase 2B-H4b: purchase payment patternのconstexpr表引き（棄却）

測定日: 2026-09-02

関連成果物:

- `phase2b_payment_pattern_table_rejected_20260902.csv`: 正式4 workloadの22-pair集計
- `phase2b_payment_pattern_table_rejected_evidence_20260902.json`: 全pair、固定slot、
  build identity、semantic recordを含むcompact evidence
- `raw/phase2b/phase2b-h4-payment-formal-rejected-*-20260902.json.gz`: 作業時raw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H4b — purchase payment patternのconstexpr表引き
Baseline commit: 8cba194（Phase 2B-H4a採用後）
Working commit: 実装はrevertし、この棄却記録のみをcommit
Branch: perf/codex56-engine-hotpaths
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3
Build flags: Release, -O3 -DNDEBUG, C++17, portable, H1/H2/H4a=ON
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは実験用`CSPLENDOR_PAYMENT_PATTERN_TABLE=OFF/ON`だけを変えた。22 pair /
11 crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot
crossoverである。両バイナリは1,069,552 bytes、GNU `size`のtextは914,058 bytes、
`.text` SHA-256は双方
`57e1f6ad8fe22d408810842b5023510b602b134a92c8f4389295c494e4ebf2b9`。

## 24.2 仮説

通常ルールで保有できる金は最大5個なので、90カード×金上限1〜5の合法な支払い候補を
3 bit/colorの`uint16_t`へcompile-time生成し、実効コストと不足数だけをruntime filterすれば、
5色再帰と中間`std::array` copyを削減できると予想した。

表は5,675 entry（11,350 bytes）と451 offset（902 bytes）で約12.0 KiB。カード本来の
costで生成し、bonus適用後の上限・所持gem由来の下限をfilterすることで、既存再帰と同じ
辞書順を保つ設計とした。`simple_payment_mode`と金6個以上のeditor局面は従来経路へ
fallbackした。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/move_generator.h` | experimental payment table | 5,675個のpacked patternとoffsetをconstexpr生成しfilter emit | 中。支払い生成の二経路化 |
| `CMakeLists.txt`, benchmark manifest | experiment toggle | code-identical OFF/ON軸 | なし。実験用 |
| `tests/rule_query_unit.cpp` | payment pattern oracle | 全90 card×金上限1〜5について内容・件数・順序を独立列挙と照合 | なし。実験用test |

改善幅が採用基準へ届かなかったため、上記コード・option・testは全てrevertした。commitへ
残す変更は本報告、CSV、compact evidenceだけである。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h4-payment-off -R 'rule_query_unit\|header_move_generator'` | 2/2 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h4-payment-on -R 'rule_query_unit\|header_move_generator'` | 2/2 pass |
| pattern oracle | `rule_query_unit` | 全5,675 patternの内容・segment件数・列挙順が一致 |
| reachable differential | `rule_query_unit` | 10,000局面以上、100,000合法手以上でcount/action/code/order/apply一致 |
| semantic digests | 正式4 workload | 全pairで一致 |

性能gateで棄却したため、候補実装に対する追加sanitizer/Python fullは実行していない。
revert先のH4a採用状態はASan+UBSan、TSan、Python fullを通過済みである。

## 24.5 performance

Aは表OFF、Bは表ON。rateはops/s、RSSはrunner peakの中央値。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal codes / gold payment | 849,136/s | 854,495/s | **1.0072** | **[1.0049, 1.0116]** | 5,640 | 5,656 |
| legal count / gold payment | 2.765 M/s | 2.747 M/s | 0.9942 | [0.9919, 1.0016] | 5,640 | 5,640 |
| legal codes / 250手 | 340,061/s | 338,556/s | 0.9986 | [0.9955, 1.0038] | 5,654 | 5,652 |
| random self-play apply | 2.431 M/s | 2.427 M/s | 1.0029 | [0.9914, 1.0114] | 5,640 | 5,640 |

最も有利な金支払い専用のcode materializationでも+0.72%に留まり、count、通常250手局面、
self-playは中立だった。Phase 2Bの主対象10%改善gateに対し、約12 KiBの表とfilter経路を
維持する根拠がない。

hardware perf counterは`perf_event_paranoid=4`のため取得不能。heap allocationは増えないが、
read-only data約12 KiBと各patternの5色unpack/filterを追加する。

## 24.6 semantic equality

```text
node count: 対象外（rule generation micro）
legal moves: 全正式pairで一致
TT hits/stores: 対象外
action-order digest: 00445425947da2c3 / 9e5be642d05486c3、A=B
reveal-order digest: 対象外
root visits: self-playの固定seed/action digest一致
tree size: 対象外
proof status: 対象外
```

## 24.7 結論

```text
REJECT_AND_REVERT
```

表引き自体は既存と同じ支払い集合・順序を保てたが、再帰は金上限5かつ5色固定で十分小さい。
runtime filterの固定費を含めると実用局面で意味のある改善がなく、実装を全て戻した。
次は独立仮説 **Phase 2B-H5 — code専用packed sink** を評価する。
