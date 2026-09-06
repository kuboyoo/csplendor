# Phase 2B-H2: token return countの閉形式化

測定日: 2026-09-02

関連成果物:

- `phase2b_return_count_20260902.csv`: 正式23 workloadの22-pair集計
- `phase2b_return_count_evidence_20260902.json`: 全pair、固定slot、build identity、
  semantic recordを含むcompact evidence
- `raw/phase2b/phase2b-h2-formal-codeident-20260902.json.gz`: 正式raw artifact
- `raw/phase2b/phase2b-h2-formal-extra-*.json.gz`: 境界fixtureと再測定のraw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H2 — token return countの閉形式化
Baseline commit: e7642cc（Phase 2B-H1採用後）
Working commit: このreportを含むH2 commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: commit後にcleanを確認予定。元checkoutの未commit変更には触れず、独立worktreeで作業
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, exact hash=ON,
             noble table=ON, single-pass codes=ON, instrumentation=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは`CSPLENDOR_CLOSED_FORM_RETURN_COUNT=OFF/ON`だけを変えた。22 pair / 11
crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot crossover。
両バイナリは1,044,976 bytes、GNU `size`のtextは891,422 bytesで同一、`.text`
SHA-256も双方
`363453c92501779777d838532b218c69cd82a00ac5d911b360e88b281809e6f7`。
設定値1 byte以外の実行コードを同一にして配置差を排除した。

## 24.2 仮説

正規到達局面でtoken action後に必要な返却数は0〜3であるが、count経路は各base actionに
6色の再帰composition列挙を実行していた。`available >= 1/2/3`の色数を`n1/n2/n3`とすると、
返却解数はそれぞれ`1`、`n1`、`C(n1,2)+n2`、
`C(n1,3)+n2*(n1-1)+n3`で求められる。

超過4以上のeditor局面は従来再帰へfallbackし、`MAX_MOVES`の残りcapで結果を飽和させる。
countは`generate_all()`の正確なreserveにも使われるため、count APIだけでなくAction生成と
self-playにも効果が波及すると予想した。heap、stack、Board field、公開ABIは追加しない。

最初のcompile-time分岐版はheader inline配置を変え、無関係なapply microに揺れを生じた。
正式版はvolatileなimmutable設定1 byteでOFF/ONの`.text`を同一化し、分岐コスト込みで測定した。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/move_generator.h` | `count_small_token_returns` | 超過0〜3を閉形式で飽和count、4以上は既存再帰 | 低。独立oracleと全入力比較 |
| `src/move_generator.h` | `closed_form_return_count_enabled` | code-identicalな厳密A/B設定 | 低。既定ON、OFFで旧経路 |
| `CMakeLists.txt` | build option | H2を既定ONで追加 | 低。portable既定不変 |
| `scripts/benchmark_manifest.py` | build metadata | A/B軸をallowlistしfingerprintから除外 | なし。計測補助のみ |
| `tests/rule_query_unit.cpp` | closed-form oracle | 562,500件の独立再帰比較と10,000局面超の差分 | なし。testのみ |
| `tests/test_engine_benchmark_tools.py` | manifest contract | option記録とfingerprint同一性 | なし |

H1 reportのbuild flag表記を、manifestどおり`native`から`portable`へ訂正した。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h2-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h2-on --output-on-failure -j2` | 33/33 pass |
| exhaustive return oracle | `rule_query_unit` | 15,625 available組 × excess 0..3 × cap 9 = 562,500比較、一致 |
| reachable differential | `rule_query_unit` | 10,000局面以上、100,000合法手以上でcount/action/code/order/apply一致 |
| Python full | fresh Release extensionで`python -m pytest -q` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan | Debug、H2=ON、hash verify=ON、`detect_leaks=0`でCTest | 33/33 pass、diagnostic 0 |
| TSan | Debug、H2=ON、hash verify=ONでCTest | 33/33 pass、race/warning 0 |
| exact reveal | 正式A/B、`five_moves`, depth 5 | node/order/reveal/proof digest一致 |

editor fallbackは`legal_count` 1.0007倍、digest一致で、超過4以上が閉形式へ入らないことを
確認した。initial（返却なし）は1.0028倍で中立、token-return fixtureは3.9532倍だった。

## 24.5 performance

Aは閉形式OFF、BはON。rateはops/sまたはsim/s、RSSはrunner peakの中央値。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count / 250手 | 1.170 M/s | 3.074 M/s | **2.6216** | **[2.6014, 2.6387]** | 5,670 | 5,696 |
| legal codes / 250手 | 188,170/s | 186,636/s | 0.9953 | [0.9870, 1.0007] | 5,686 | 5,680 |
| legal actions / 250手 | 169,308/s | 186,633/s | **1.1076** | **[1.0991, 1.1188]** | 5,718 | 5,688 |
| random self-play apply | 1.580 M/s | 1.833 M/s | **1.1606** | **[1.1578, 1.1715]** | 5,720 | 5,720 |
| exact legacy 1T | 297,881/s | 300,124/s | 1.0127 | [1.0013, 1.0213] | 103,164 | 103,192 |
| solver state key | 6.738 M/s | 6.742 M/s | 0.9998 | [0.9986, 1.0068] | 5,688 | 5,700 |
| visible solver | 641,797/s | 645,837/s | 0.9995 | [0.9907, 1.0128] | 12,848 | 13,006 |
| exact reveal | 512,085/s | 514,268/s | 1.0024 | [0.9966, 1.0049] | 16,220 | 16,262 |

正式suiteのBoard copyは0.9827だったが、OFF/ONが同一命令列であるため独立22-pairを再実行し、
0.9983、CI `[0.9803, 1.0116]`で中立を確認した。determinization cloneも独立再測定で
0.9994、CI `[0.9986, 1.0002]`。他の主要workloadの中央値悪化は2%以内だった。

Python境界の8独立process ABBA/BAAB交差測定は、`legal_action_codes` 1.0209倍、
`legal_action_count` 1.0121倍、`legal_actions` 1.0160倍、C++ playout 1.1006倍。

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
| allocations | unchanged | unchanged | count経路にallocationなし |

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

全23 workloadと境界fixtureでdigest、correctness counterが一致した。closed formは件数だけを
返し、Action emit順序には触れない。

## 24.7 結論

```text
ACCEPT
```

legal count +162.16%、legal actions +10.76%、self-play +16.06%で全gateを通過し、
legal codesは-0.47%、Python legal actionsは+1.60%。全oracle、sanitizer、探索量・順序・
digestを維持し、追加allocationなしのため採用する。

残存リスクは、editor由来の超過4以上では従来再帰のままで高速化されないことと、OFF/ON厳密
A/Bのため1回のvolatile分岐が残ること。いずれも正式測定の利益に含まれる。

commit hashは最終回答に記載する。次に実行すべき独立仮説は
**Phase 2B-H3 — purchase payment count専用DP**のみとする。
