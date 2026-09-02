# Phase 2B-H1: 合法手codeの単回列挙

測定日: 2026-09-02

関連成果物:

- `phase2b_single_pass_codes_20260902.csv`: 正式6 workloadと境界6 fixtureの集計
- `phase2b_single_pass_codes_evidence_20260902.json`: 全pair、固定slot、build identity、
  semantic recordを含むcompact evidence
- `/tmp/phase2b-h1-formal-standard-20260902.json`: 正式22-pair raw artifact
- `/tmp/phase2b-h1-exact-reveal-semantic-20260902.json`: exact reveal raw artifact

## 24.1 状態

```text
Target phase: Phase 2B-H1 — 合法手code生成の単回列挙
Baseline commit: 5644cbbb00c27b63d095030d0b8c50f16963693c
Working commit: このreportを含むH1 commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: commit後にcleanを確認予定。元checkoutの未commit変更には触れず、独立worktreeで作業
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, exact hash=ON,
             noble table=ON, instrumentation=OFF, hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは同一sourceから`CSPLENDOR_SINGLE_PASS_LEGAL_CODES=OFF/ON`だけを
変えた。22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、ABBA、
固定inode 2-slot crossoverで測定した。benchmark build fingerprintは双方
`55447a1b7ffd0cd8ab442e81503c6f239d0f204383eed15b29cfaae4c26ab988`。

A binary SHA-256は
`a59bf50ede1562e169fae89b8e800c7c5a70eb59ad3b6f959e0544f0ef9da5a4`、
Bは`3b4875baa496ac820f1d1aae356e497f8a22e55bc2f2684cfb5ac2fd48d8837d`。
GNU `size`のtext categoryは891,890から891,150 bytesへ740 bytes減少した。

## 24.2 仮説

`Game::legal_action_codes()`は、正確なvector capacityを得るために
`count_all_fixed()`で全合法手を数えた後、同じ合法手を再度列挙してpackしていた。
Phase 0 profileでもcode sinkとreturn展開を含む二重列挙がlegal generation上位に現れた。

最大公開手数は既存契約の`MAX_MOVES=2048`で固定されている。未初期化の16 KiB固定scratchへ
1回だけpackし、実要素範囲から結果vectorを構築すれば、返却vector以外のheap allocationを
増やさず、列挙・支払い・返却生成の前半を完全に除去できる。5手局面、250手局面、
返却、gold支払い、simple payment、editor fallbackを個別測定した。

試作した固定scratch、`reserve(64)` vector、cheap size predictorのうち、固定scratchは
5手局面以外の全fixtureで最速で、5手局面でも旧実装比1.70倍だったため採用した。
hardware cycles割合は`perf_event_paranoid=4`により取得不能。hot pathの追加heap allocationは0、
一時stackは16 KiB、公開ABIと`Board`/`Game` layoutは不変。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/game.h` | `Game::legal_action_codes` | count前走査を除き、固定scratchへ1回だけ列挙・pack | 低。既存emitter、上限、順序をそのまま使用 |
| `CMakeLists.txt` | `CSPLENDOR_SINGLE_PASS_LEGAL_CODES` | 厳密A/B用optionを追加し既定ON | 低。OFFで旧実装を保持 |
| `scripts/benchmark_manifest.py` | build metadata | A/B軸をallowlistしfingerprintから除外 | なし。計測補助のみ |
| `tests/rule_query_unit.cpp` | large reachable corpus | 10,000局面超でcount/action/code/order/apply結果を差分検証 | なし。testのみ |
| `tests/test_engine_benchmark_tools.py` | manifest contract | option記録とA/B fingerprint同一性を検証 | なし |

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2b-h1-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2b-h1-on --output-on-failure -j2` | 33/33 pass |
| reachable differential | `rule_query_unit` | 10,000局面以上、100,000合法手以上でcount/actions/codes/order/apply snapshot一致 |
| Python full | fresh Release extensionで`python -m pytest -q` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan | Debug、H1=ON、hash verify=ON、`detect_leaks=0`でCTest | 33/33 pass、diagnostic 0 |
| TSan | Debug、H1=ON、hash verify=ONでCTest | 33/33 pass、race/warning 0 |
| exact reveal | A/B、`five_moves`, depth 5, node limit 1,000,000 | node/order/reveal/digest一致 |

通常reachable corpusに加えて、強制PASS、waiting noble、`MAX_MOVES` cap、editor由来の
excess>3 fallbackは既存unit/境界fixtureで通過した。V2/V3 encoderの回帰を含む全CTestも
A/B双方で一致した。

## 24.5 performance

Aは単回列挙OFF、BはON。rateはops/s、RSSはrunner peakの中央値。
標準表は22 pair、CIは11 crossover blockのbootstrap 95% CI。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count / 250手 | 1.132 M/s | 1.129 M/s | 0.9989 | [0.9956, 1.0033] | 5,680 | 5,556 |
| legal codes / 250手 | 149,547/s | 182,225/s | **1.2134** | **[1.2110, 1.2325]** | 5,680 | 5,678 |
| legal actions / 250手 | 168,529/s | 169,270/s | 1.0045 | [0.9705, 1.0173] | 5,678 | 5,552 |
| random self-play apply | 1.625 M/s | 1.627 M/s | 0.9984 | [0.9891, 1.0128] | 5,710 | 5,710 |
| exact legacy 1T | 307,717/s | 306,725/s | 0.9941 | [0.9859, 1.0029] | 103,128 | 103,136 |
| Board copy/restore | 12.034 M/s | 12.065 M/s | 1.0080 | [0.9944, 1.0339] | 5,680 | 5,682 |

境界fixtureのlegal codesは、5手局面1.6952倍、initial 1.6001倍、token return
1.2314倍、gold payment 1.3087倍、simple payment 1.3096倍、editor fallback
1.1905倍で、全CIが1を上回った。Python bindingは独立processのABBA/BAAB交差順序で
再測定し、`legal_action_codes` 1.2871倍、`legal_actions` 0.9876倍（-1.24%）だったため
Python退行上限2%も満たした。

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
| allocations | 返却vector 1 | 返却vector 1 | heap allocation増加なし |

## 24.6 semantic equality

```text
node count: exact reveal 139,868、A=B
legal moves: exact reveal 692,386、A=B
TT hits/stores: memo hits 17,163 / memoized states 67,615、A=B
action-order digest: legal codes 9e5be642d05486c3、A=B
reveal-order digest: exact reveal digest 9dd9c11919a4581a、A=B
root visits: legacy workloadのcounter/digest一致
tree size: legacy workloadのcounter/digest一致
proof status: exact revealのstatus/reason/limit一致
```

各境界fixtureでもordered code digestがA/Bで一致した。探索量、TT、deck reserve候補、
forced pass、上限打切りのcorrectness counterも全pairで一致した。

## 24.7 結論

```text
ACCEPT
```

primaryの250手legal codesは+21.34%、5手局面は+69.52%で各gateを超え、
Python `legal_actions`退行は1.24%に留まった。全oracle、sanitizer、順序、上限、
探索digestが一致し、binary textも740 bytes減ったため採用する。

残存リスクは呼出し中の16 KiB stack使用である。scratchは関数復帰前にvectorへ移され、
再帰フレームを跨いで保持されない。`MAX_MOVES`変更時にはstack量も追随する。

commit hashは最終回答に記載する。次に実行すべき独立仮説は
**Phase 2B-H2 — token return countの閉形式化**のみとする。
