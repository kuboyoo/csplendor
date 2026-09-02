# Phase 1A: exact Zobrist hashの差分維持

測定日: 2026-09-02

関連成果物:

- `phase1a_paired_20260902.csv`: 15 workloadの正式paired A/B集計
- `phase1a_paired_evidence_20260902.json`: 全pair比、crossover block比、
  binary/build identity、実行command、代表semantic record
- `raw/phase1a/csplendor-phase1a-release-off-on-differential-1000.json.gz`:
  1,000局面の作業時差分検証原本

## 24.1 状態

```text
Target phase: Phase 1A — exact Zobrist hashの差分維持
Baseline commit: 6d36e8a8b66735ff7294896d24d1b4b2a2bfa7de
Working commit: このreportを含むPhase 1A commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: commit後にcleanを確認予定。元checkoutの未commit変更には触れず、独立worktreeで作業
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, instrumentation=OFF, verify=OFF
CPU affinity: 1T=CPU 4、4T=CPU 4-7、8T=CPU 4-11（各set内にSMT siblingなし）
```

正式A/Bの唯一のengine build差は
`CSPLENDOR_INCREMENTAL_EXACT_HASH=OFF/ON`である。両方とも
`CSPLENDOR_VERIFY_INCREMENTAL_HASH=OFF`で、manifestのbuild fingerprintは
`55447a1b7ffd0cd8ab442e81503c6f239d0f204383eed15b29cfaae4c26ab988`
に一致した。実験軸のOFF/ON値はfingerprintから除外する一方、各manifestへ明示保存した。

binary SHA-256はAが
`d244e3fa12d364efcc35338a1bee9649f478b037a9a196b33c98d91274248834`、
Bが`2b20a66130e65c00516b3bcbeb545d18da74bff31b4f4f2876f6e2973fff4fa8`。
固定inode 2スロットをpairごとに交差し、22 pair / 11 crossover block、各側3 warmup、
10,000 bootstrapで測定した。

環境はRyzen 9 7900X（12 core / 24 thread）、Linux
7.0.0-30-generic x86_64。CPU 4のgovernorは`performance`、boostは有効。
governorと`perf_event_paranoid`は変更していない。

## 24.2 仮説

Phase 0では、`apply_only` 21.37 M/sに対して`apply_exact_hash`は
10.92 M/s、cold exact hash 16.80 M/sに対してcached hashは165.44 M/sだった。
instrumentationでも探索遷移後のexact hashはほぼ全てcache missとなり、board fieldと
残存deckを毎node走査していた。

したがって、trusted transition開始時点でexact cacheがvalidなら、変更fieldのold saltを
XOR-outしnew saltをXOR-inして、成功時だけcandidate hashをpublishする。これにより
full-board/deck scanを除去し、exact MCTSを短縮できると予想した。cache invalid、editor、
任意位置deck eraseは従来どおりinvalidateしてfallbackするため、hashを使わないself-playの
追加費用はcompile-timeで除去できると見込んだ。

hardware cycles割合は取得不能だった。`perf stat`は
`perf_event_paranoid=4`により`No supported events found`で終了したため、設定を変更せず、
Phase 0のthroughput/counterをhotness根拠とした。allocationはhash scan自体の原因ではなく、
instrumentation A/Bでも同数だった。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `CMakeLists.txt` | build options | 増分hash既定ON、compile-time oracle既定OFFを追加 | 低。portable既定と既存targetを維持 |
| `src/board.h` | `Board::RuleMutator`、exact salt helper | stack-local candidate hash、field/deck/noble/current-player等の型付きdelta、成功時commit、失敗時invalidate | 中。hash対象fieldの網羅性を独立oracleで検証 |
| `src/game.h` | `Game::apply_unchecked`系 | valid cacheだけ増分mutator、invalid cacheは完全compile-outした非hash mutatorへdispatch | 中。着手規則本体は単一templateで共有 |
| `src/rule_transition.h` | rule transition helpers | 既存raw overloadを保持し、mutator overloadを追加 | 低。solver/editor互換を維持 |
| `src/reveal_verified_solver.cpp` | 任意位置deck erase | position saltが後続全て変わる経路を明示invalidate | 低。従来fallbackを明文化 |
| `scripts/benchmark_engine_hotpaths.cpp` | transition corpus、`apply_exact_hash`、`root_parallel` | cache preconditionをA/Bで統一し、既存root-parallel実装のJSONL formal workloadを追加 | 低。既存CSV/APIは不変 |
| `scripts/benchmark_manifest.py` | CMake allowlist/fingerprint | 増分hash/verify optionを証跡へ保存し、増分hashだけを明示A/B軸として扱う | 低。秘密値を読む範囲は拡張しない |
| `tests/incremental_hash_unit.cpp` | random differential/oracle | 1,000終局trajectory、全合法後継、editor/clone/snapshot/determinization/undo/failureを検証 | なし。testのみ |
| `tests/CMakeLists.txt` | native tests | `incremental_hash_unit`を登録 | なし |
| `tests/test_engine_benchmark_tools.py` | manifest/root-parallel contracts | A/B軸記録、root result/digestの厳密比較を検証 | なし |
| `doc/performance_experiments/*phase1a*` | evidence/report | 正式paired A/Bと採否根拠を保存 | なし |

`Board`の公開data fieldは追加・並べ替えをしていない。Phase 0 / A / Bで
`sizeof(Board)=392`、`sizeof(Game)=448`、layout digest
`210248ca4912bb94`が一致した。editor/raw path、Python binding、既存
`rule_transition` overloadも維持した。

増分transactionは途中状態を外部へvalid cacheとして公開せず、constructorでcacheを
invalidateしてcandidateをstackへ退避し、successful transitionの`commit()`だけが
`cached_hash/hash_valid`をpublishする。例外、early return、部分失敗ではinvalidに倒す。
cacheが元からinvalidならsalt lookup自体を`if constexpr`で除去する。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase1a-release-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase1a-release-on --output-on-failure -j2` | 33/33 pass |
| incremental oracle | `/tmp/csplendor-phase1a-release-on/tests/incremental_hash_unit` | pass、441,464 oracle checks、failure 0 |
| 1,000-state A/B | `python scripts/validate_engine_differential.py --baseline-binary ...off... --candidate-binary ...on... --seeds 1000 --fixture-plies 32 ...` | 12,000 semantic records一致、6,000 exact oracle、failure 0 |
| Python full | isolated fresh Release extensionで`python -m pytest` | 556 pass、2 skip、4 deselect |
| Python performance | isolated fresh Release extensionで`python -m pytest -m performance` | 4 pass、558 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan+oracle | Debug、incremental=ON、verify=ON、`CSPLENDOR_SANITIZER=address-undefined`でCTest | 33/33 pass、diagnostic 0 |
| TSan+oracle | Debug、incremental=ON、verify=ON、`CSPLENDOR_SANITIZER=thread`でCTest | 33/33 pass、race/warning 0 |
| instrumentation | Release、incremental=ON、instrumentation=ONでCTest | 33/33 pass |
| exact reveal digest | A/Bで`exact_reveal`, `five_moves`, depth 5, node limit 1,000,000 | node/order/reveal/digest一致 |

`incremental_hash_unit`の内訳は1,000 games、57,440 trajectory transitions、
1,941 all-legal states、50,087 all-legal successors、441,464 independent oracle checks。
全7 ActionType、simple/full payment、blank refill、visible/deck reserve、hidden reserve購入、
token return/gold、貴族0/1/複数、forced PASS/draw、final round、editor、full/light clone、
snapshot、determinization、undo、失敗transitionを含む。valid-cache shadowとinvalid-cache
shadowで全Board field、derived/provenance field、ordered legal listを比較した。

別processの1,000-state differentialはsetup action 31,998、legal action 23,617、
corpus digest
`cc630947b8455fa6b8fdeba587065e40eb80359149d58c62030cac1e84694bb8`
がA/Bで一致した。

## 24.5 performance

正式結果。Aは増分hash OFF、BはON。rateはops/sまたはsim/s、RSSはrunner peakの
中央値。全行22 pairの固定slot crossoverで、CIは11 blockのbootstrap 95% CI。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| cold hash | 15.165 M/s | 15.059 M/s | 0.9921 | [0.9806, 1.0077] | 5,664 | 5,686 |
| cached hash | 166.243 M/s | 166.454 M/s | 1.0009 | [0.9966, 1.0031] | 5,664 | 5,688 |
| apply only | 20.057 M/s | 19.924 M/s | 0.9889 | [0.9836, 0.9935] | 7,594 | 7,662 |
| apply + exact hash | 9.895 M/s | 16.263 M/s | **1.6467** | **[1.6407, 1.6608]** | 7,740 | 7,768 |
| exact legacy 1T | 288,959 | 313,366 | **1.0844** | **[1.0775, 1.0877]** | 103,138 | 103,152 |
| exact root-parallel 4T | 785,639 | 820,473 | 1.0538 | [1.0245, 1.1872] | 62,240 | 62,352 |
| exact root-parallel 8T | 1,255,675 | 1,305,617 | 1.0363 | [1.0227, 1.0545] | 63,816 | 63,864 |
| exact sharded 1T | 229,353 | 242,575 | 1.0581 | [1.0526, 1.0613] | 44,114 | 44,144 |
| exact sharded 4T | 180,312 | 180,940 | 1.0044 | [0.9782, 1.1426] | 44,174 | 44,228 |
| exact sharded 8T | 348,613 | 351,487 | 0.9838 | [0.8076, 1.1830] | 43,896 | 43,946 |
| determinized legacy 1T | 266,937 | 265,436 | 0.9973 | [0.9890, 1.0046] | 114,552 | 114,556 |
| random self-play apply | 1.625 M/s | 1.629 M/s | 1.0022 | [1.0012, 1.0035] | 5,646 | 5,714 |
| legal count | 1.203 M/s | 1.221 M/s | 1.0157 | [0.9649, 1.0227] | 5,664 | 5,554 |
| legal codes | 155,898 | 160,099 | 1.0257 | [1.0229, 1.0288] | 5,664 | 5,690 |
| legal actions | 170,576 | 169,026 | 0.9935 | [0.9923, 0.9964] | 5,664 | 5,580 |

primary acceptanceは`apply + exact hash`の+64.7%とexact legacyの+8.44%で、
どちらもCI下限1.01を超えた。hashを使わないcontrolの最大中央値低下は
`apply_only`の-1.11%、legalは-0.65%、self-playは+0.22%で、2% gate内。
determinized legacyは-0.27%（CI下限-1.10%）で非劣化gate内だった。
shared sharded 4T/8Tはscheduler競合ノイズが大きく、改善量は確定できないが、
primary採用判定には使っていない。

instrumentationはobserver effectがあるため正式throughputには使わず、機序確認だけに
使った。

| counter | apply+hash A | apply+hash B | legacy A | legacy B |
|---|---:|---:|---:|---:|
| exact hash calls | 2,000,000 | 2,000,000 | 286,731 | 286,731 |
| cache hits | 0 | 2,000,000 | 4,096 | 286,731 |
| cache misses | 2,000,000 | 0 | 282,635 | 0 |
| fields visited | 131,671,883 | 0 | 18,653,910 | 0 |
| deck salts visited | 124,023,444 | 0 | 20,240,301 | 0 |
| hash oracle failures | 0 | 0 | 0 | 0 |
| allocation calls | 671,879 | 671,879 | 1,242,192 | 1,242,192 |
| allocation bytes | 10,578,135 | 10,578,135 | 213,691,287 | 213,691,287 |

`benchmark_engine_hotpaths`のfile sizeは1,026,520から1,044,272 bytes
(+1.73%)、`.text`は753,578から763,178 bytes (+1.27%)。通常のengine/parallel
binaryではdata/bssは不変。fresh-process `apply_exact_hash` 11回のRSS監査でも
中央値差は+76 KiB (+0.99%)だった。

hardware perf counter:

| metric | A | B | delta |
|---|---:|---:|---:|
| cycles | N/A | N/A | `perf_event_paranoid=4` |
| instructions | N/A | N/A | 同上 |
| IPC | N/A | N/A | 同上 |
| branch misses | N/A | N/A | 同上 |
| L1D misses | N/A | N/A | 同上 |
| LLC misses | N/A | N/A | 同上 |
| atomic RMW | unchanged search contract | unchanged search contract | hash deltaにatomicなし |
| allocations | 同数 | 同数 | 0 |

正式runnerの代表commandは次の形。全15 caseの展開済みA/B command、全pair比、binary
identityはcompact evidence JSONに保存した。

```bash
python scripts/run_phase0_baseline.py \
  --baseline-binary /tmp/csplendor-phase1a-release-off/benchmark_engine_hotpaths \
  --candidate-binary /tmp/csplendor-phase1a-release-on/benchmark_engine_hotpaths \
  --baseline-repo-root /tmp/csplendor-codex56-phase0 \
  --candidate-repo-root /tmp/csplendor-codex56-phase0 \
  --output-json doc/performance_experiments/raw/phase1a/phase1a_primary_guards_rerun.json \
  --output-csv doc/performance_experiments/raw/phase1a/phase1a_primary_guards_rerun.csv \
  --pairs 22 --warmups 3 --bootstrap-iterations 10000 \
  --cpu-set 4 --timeout 300 \
  --case legal_count --case random_selfplay_apply --case apply_only \
  --case apply_exact_hash --case cold_hash --case cached_hash \
  --case legacy_mcts

python scripts/run_paired_benchmarks.py paired \
  --baseline-command '<A binary> --workload root_parallel --fixture midgame_250 --iterations 49152 --warmup 4096 --threads 8 --batch-size 16 --determinization false' \
  --candidate-command '<B binary> --workload root_parallel --fixture midgame_250 --iterations 49152 --warmup 4096 --threads 8 --batch-size 16 --determinization false' \
  --pairs 22 --warmups 3 --bootstrap-iterations 10000 \
  --rotate-binary-slots --cpu-set 4-11 --timeout 300
```

## 24.6 semantic equality

```text
node count: exact reveal 139,868 / legacy evaluated boards 65,536、A=B
legal moves: exact reveal 692,386、midgame fixture ordered legal count 250、A=B
TT hits/stores: exact reveal memo hits 17,163 / memoized states 67,615、A=B
action-order digest: edfd4c52f12a9c7a、A=B
reveal-order digest: 65bbe0573367e0a1、A=B
root visits: legacy 65,520、root-parallel 4T/8T 49,152、A=B
tree size: legacy 65,521、root-parallel 4T 49,156 / 8T 49,160、A=B
proof status: proven=false, reason=candidate_mate_not_verified, limit_reached=false、A=B
```

exact reveal全体digestは`9dd9c11919a4581a`、principal-line action digestは
`ee6f8466fd88410b`、principal-line reveal digestは`70ce0d9a743220ca`で一致した。
legacy MCTS全体digestは`0ab37fc767a05581`。formal traceもexpanded key
`28524f468412a563`、selected action `441256b04b533e55`、inference request
`3dfca9ea306a2051`、request replay `5902e441bca11f15`でA/B一致した。

root-parallel harnessはworker budget、merged root visits/tree size、virtual loss、ledger、
selected action legalityを検証し、4T/8Tのroot visit/Q/worker/probability digestをA/Bで
厳密比較した。全paired caseでcorrectness digestとrequired semantic metadataが一致した。

## 24.7 結論

```text
ACCEPT
```

採用理由は、`apply + exact hash`が+64.7%、exact legacy MCTSが+8.44%で、
両primary workloadの95% CI下限が1.01を超えたこと、self-play/legal/controlの
中央値回帰が2%以内、hash oracle failure 0、公開layout/API・探索量・順序・digestが
不変だったため。

残存リスクは、header実装量と`.text`が増えたこと、shared sharded 4T/8Tでは並列ノイズが
大きく単独効果が確定していないこと、hardware perf eventを権限制約で取得できなかったこと。
ただし正式primary、RSS、sanitizer、differentialの採用gateは全て通過した。

commit hashは最終回答に記載する。次に実行すべきPhaseは**Phase 1B — observable
hashの差分維持**のみとする。
