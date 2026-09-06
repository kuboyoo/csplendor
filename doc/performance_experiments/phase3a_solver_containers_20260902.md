# Phase 3A: solverの低リスクcontainer/metadata削減

測定日: 2026-09-02

関連成果物:

- `phase3a_solver_containers_20260902.csv`: 正式23 workloadの集計
- `phase3a_solver_containers_evidence_20260902.json`: 全正式pair、独立仮説、
  固定slot、build identity、semantic recordを含むcompact evidence
- `phase3a_solver_containers_diagnostics_20260902.json`: allocation、path深さ、
  適応方式およびraw artifact SHAの要約
- `raw/phase3a/phase3a_solver_containers_final_20260902.json.gz`: 正式raw artifact

## 1. 状態

```text
Target phase: Phase 3A — solver低リスクcontainer/metadata削減
Baseline commit: b2b23aa（Phase 2B完了）
Working commit: このreportを含むPhase 3A commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable、Phase 1/2採用案=ON、
             instrumentation=OFF、hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/BはPhase 3Aの4 optionをすべてOFF/ONとし、それ以外を一致させた。22 pair / 11
crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot crossoverである。
A/B benchmark binaryは1,070,464 / 1,069,472 bytes、GNU `size`のtextは
905,514 / 903,794 bytes、SHA-256は
`92a85801750b561fd2060dccc8155b9e493012689d31ce8bce43d40f7c32117b` /
`2e6c6cd03bf1b07593cd488b8a02c99ff4fcb922008afc72c29340629d21486c`。

## 2. 実装と判断

| hypothesis | implementation | compatibility risk | result |
|---|---|---|---|
| bounded recursion path | `RecursionPath`の連続stack、逆順linear scan、RAII push/pop、初回reserve | 低。旧hash setをOFF oracleとして保持 | 採用 |
| unbounded visible minimax path | 深さ診断に基づきhash setを維持 | 低。従来と同じmembership | 適応方式を採用 |
| card equivalence | 90枚をconstexpr class ID化し、2-word maskで既出管理 | 低。90×90双方向oracleあり | 採用 |
| forced attacker actions | 出力vector 1本と上位6 take / 3 reserveの固定array | 低。category、cap、比較順を維持 | 採用 |
| TT reason | `std::string`を4値enumにし、公開境界で従来文字列へ戻す | 低。公開文字列oracleあり | 採用 |
| map single lookup | `find`後の代入を`try_emplace`系へ変更 | 中。TT更新規則に接触 | 中立のため完全revert |

深さ制限付きの詰み探索はpath長が最大9〜11だったためstackが有効だった。一方、深さ制限のない
visible minimaxはtoken cycleを含み最大108 plyへ達した。ここまで一律linear scanにすると、
`forced_pass`の統合効果は1.0194倍、CI [0.9712, 1.0766]に留まった。そのため無制限minimaxだけ
hash setへ戻し、bounded forced searchだけstackを使用する方式にした。最終結果は同fixtureで
1.0802倍、CI [1.0758, 1.0843]となった。

card tupleは`level/points/bonus/cost`で分類する。現行90枚は偶然すべて別classだが、従来の
`std::set<CardEquivalenceKey>`を枝ごとに構築せず2個の`uint64_t`だけで同じ判定を行える。
将来カードデータに同tupleが加わってもclass生成とmaskはそのまま機能する。

## 3. correctness

| test | command / scope | result |
|---|---|---|
| Release A | Phase 3A全option OFF、CTest | 33/33 pass |
| Release B | Phase 3A全option ON、CTest | 33/33 pass |
| component oracle | path RAII、90×90 class/tuple、forced-action cap/order | pass |
| Python full | fresh Release extensionで`python -m pytest` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| performance tests | `python -m pytest -m performance` | 4 pass、558 deselect |
| 5手・7手詰み | known 5/7手およびparallel iterative search | 4 pass |
| ASan+UBSan | Debug、全option ON、hash verify=ON、leak検査のみ無効 | 33/33 pass、diagnostic 0 |
| TSan | Debug、全option ON、hash verify=ON | 33/33 pass、race/warning 0 |

ASanの初回CTestはptrace環境下のLeakSanitizer起動制約だけで失敗した。
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`で再実行し、ASan/UBSan本体は全件通過した。

正式23 workload、独立仮説、`forced_pass`再測定の全pairでdigestとcorrectness counterが一致した。
強制PASS、貴族待ち、公開reason文字列、action category/order、厳密めくれ候補順を変更していない。

## 4. 正式performance

AはPhase 3A全option OFF、Bは全option ON。rateはops/s、sim/sまたはnodes/s、RSSはrunner
peakの中央値である。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| exact reveal / five moves | 503,355 | 643,817 | **1.2786** | **[1.2662, 1.2979]** | 16,310 | 16,360 |
| visible solver / five moves | 689,650 | 706,817 | **1.0194** | **[1.0172, 1.0261]** | 13,008 | 12,988 |
| exact legacy MCTS | 290,051 | 294,215 | 1.0092 | [1.0016, 1.0216] | 103,182 | 103,182 |
| shared tree MCTS | 3,533,291 | 3,429,316 | 0.9929 | [0.9725, 1.0401] | 5,704 | 5,724 |
| legal actions | 340,722 | 337,947 | 0.9970 | [0.9790, 1.0100] | 5,706 | 5,704 |
| decode + apply | 16.379 M | 15.986 M | 0.9838 | [0.9664, 0.9969] | 7,792 | 7,802 |

厳密めくれ探索は27.86%改善し、Phase 3Aの3〜10%採用gateを超えた。`decode_apply`は
1.62%低下したが2%退行gate内である。23 workloadのうちCI上限が0.98未満となるものはなく、
2%超の確定退行はない。最初のfull suiteで変動したshared treeは44 pairの独立再測定で
1.0130倍、CI [1.0076, 1.0328]となり、Phase 3A由来の退行ではないことを確認した。

## 5. 独立仮説

各行は対象optionだけをOFF/ONにした22 pair / 11 crossover blockである。

| hypothesis / workload | A median | B median | B/A | 95% CI |
|---|---:|---:|---:|---:|
| path stack / visible five moves | 682,761 | 693,800 | **1.0161** | [1.0105, 1.0212] |
| path stack / exact reveal | 513,058 | 516,221 | 1.0068 | [0.9895, 1.0153] |
| card classes / exact reveal | 502,572 | 618,942 | **1.2392** | [1.2198, 1.2421] |
| compact actions / visible five moves | 651,292 | 667,896 | **1.0090** | [1.0036, 1.0416] |
| compact actions / exact reveal | 599,974 | 612,191 | **1.0227** | [1.0006, 1.0294] |
| compact reasons / forced pass | 1,991,778 | 2,167,275 | **1.0769** | [1.0644, 1.1117] |
| map single lookup / visible five moves | 693,742 | 698,955 | 1.0034 | [0.9952, 1.0208] |

map案は中央値+0.34%でもCIがneutralを含み、複雑性に見合わないためコードを完全に戻した。
採用コードにはこの仮説の変更は残っていない。

## 6. allocation、RSS、path深さ

instrumentationはwarmup後にresetし、正式と同じ操作を1回実行した。

| workload | metric | A | B | delta |
|---|---|---:|---:|---:|
| exact reveal | global allocation calls | 3,211,048 | 1,281,407 | -60.1% |
| exact reveal | allocated bytes | 279,933,253 | 140,916,189 | -49.7% |
| exact reveal | temporary set markers | 64,958 | 1 | -99.998% |
| visible five moves | global allocation calls | 2,679,065 | 2,554,415 | -4.7% |
| visible five moves | allocated bytes | 85,234,562 | 69,589,010 | -18.4% |
| visible forced pass | runner RSS | 30,130 KiB | 17,808 KiB | -40.9% |
| visible forced pass | allocated bytes | 219,629,834 | 208,674,893 | -5.0% |

exact reveal pathは67,615 sample、平均8.68、最大9、linear comparison 586,852だった。
visible five-move pathは41,035 sample、平均9.41、最大11だった。`forced_pass`全体は最大108だが、
適応方式では無制限minimaxをhash setに残すためlinear comparisonは74,803だけである。
この結果はstack化の適用範囲を「有界深さ」とする根拠になった。

exact revealの正式peak RSSはTT等に支配され50 KiB増で中立だったが、allocation callとbytesは
大幅に減った。大量のTT entryへreasonを保存するvisible `forced_pass`では、enum化により
runner RSSが約12 MiB減った。

## 7. semantic equality

```text
exact reveal nodes: 139,868、A=B
exact reveal legal moves: 692,386、A=B
exact reveal memo hits/states: 17,163 / 67,615、A=B
exact reveal digest: 9dd9c11919a4581a、A=B
exact principal line action digest: ee6f8466fd88410b、A=B
exact principal line reveal digest: 70ce0d9a743220ca、A=B
visible five-move nodes: 100,000、A=B
visible five-move digest: b6cdbdc80156e463、A=B
visible forced-pass nodes: 1,000,000、A=B
visible forced-pass digest: 46ac22ac2ea2fecf、A=B
public status/reason/unknown_reason/winning action: A=B
```

hardware perf counterは`perf_event_paranoid=4`のため取得不能である。追加atomic、外部依存、
Board layout、公開binding、Action wire formatは変更していない。

## 8. 結論

```text
ACCEPT
```

厳密めくれ探索を27.86%、長時間visible探索を8.02%高速化し、`forced_pass`のRSSを40.9%、
exact revealのallocation callを60.1%削減した。探索結果・候補順・公開文字列は同一で、全回帰、
5/7手詰み、sanitizer、semantic gateを通過したため、4案を既定ONで採用する。Phase 3Aを完了し、
次の独立作業単位はPhase 3Bである。
