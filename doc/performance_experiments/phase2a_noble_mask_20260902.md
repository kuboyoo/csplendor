# Phase 2A-1: 貴族適格maskのtable化

測定日: 2026-09-02

関連成果物:

- `phase2a_noble_mask_paired_20260902.csv`: 正式8 workloadの22-pair集計
- `phase2a_noble_mask_paired_evidence_20260902.json`: 全pair、固定slot、build
  identity、semantic recordを含むcompact evidence
- `raw/phase2a/phase2a1-formal-v3-20260902.json.gz`: 作業時raw artifact
- `raw/phase2a/phase2a1-differential-final-1000-20260902.json.gz`: 1,000局面差分検証原本

## 24.1 状態

```text
Target phase: Phase 2A-1 — 貴族適格maskのtable化
Baseline commit: a5fbd21336aa8620a31daeca17aa5b514c513a05
Working commit: このreportを含むPhase 2A-1 commit（SHAは最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Working tree status: commit後にcleanを確認予定。元checkoutの未commit変更には触れず、独立worktreeで作業
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, exact hash=ON,
             instrumentation=OFF, hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは同一sourceから`CSPLENDOR_NOBLE_ELIGIBILITY_TABLE=OFF/ON`だけを
変えた。22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、ABBA、
固定inode 2-slot crossoverで測定した。build fingerprintは双方
`55447a1b7ffd0cd8ab442e81503c6f239d0f204383eed15b29cfaae4c26ab988`。

A binary SHA-256は
`ee3a764f29ac849cddc7265b75b9b845df07294d7a4b235f5903e470af5b1679`、
Bは`a59bf50ede1562e169fae89b8e800c7c5a70eb59ad3b6f959e0544f0ef9da5a4`。
いずれも1,045,144 bytesで、GNU `size`のtext categoryは891,890 bytes、実`.text`
sectionは765,866 bytes、SHA-256
`37dd984ac1b80bbf1ea80a41b7d43d11213bcadb0ea9c6223c46ea3629a8b71f`と
完全一致した。OFF/ON差はC++17 inline storageのimmutable設定1 byteだけである。

環境はRyzen 9 7900X、Linux 7.0.0-30-generic x86_64。governor、boost、
`perf_event_paranoid`は変更していない。

## 24.2 仮説

購入時のbonus更新後、現行は全12貴族についてpacked requirementとの差を5色ずつ
`needed_gold()`で再計算していた。購入専用smoke microではこの処理を色別constexpr
mask 5回のANDへ置換すると約23%改善し、十分な局所hotspotであることを確認した。

各色について「そのbonus数で要求を満たす貴族bit」をcompile-time tableへ持ち、5色の
ANDだけにすれば、12貴族×5色の判定loopを除去できる。最大要求4を超えるeditor値は4へ
clampしても全要求を満たすため意味論は変わらない。

最初のheader compile-time分岐試作は購入+30.5%だった一方、無関係なlegal countに
-1.86%のcode-layout回帰が出たため正式結果には採用しなかった。両経路を同一`.text`へ
置き、設定byteだけを変える測定形へ修正後、以下の正式A/Bを取得した。

hardware cycles割合は取得不能だった。`perf stat`は
`perf_event_paranoid=4`により`No supported events found`で終了した。表はconstexprで
静的領域のみ、hot pathのallocationは追加しない。

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/noble_data.h` | `NOBLE_MASK_BY_COLOR` | 5色×bonus 0..4のconstexpr mask、clamp、全組合せstatic assertion | 低。独立referenceと全組合せ検証 |
| `src/player.h` | `update_noble_eligibility`、inline config | 12貴族scanを5 table lookup/ANDへ置換し、OFF/ONの`.text`を同一化 | 低。公開field/API/layoutとheader-only利用不変 |
| `CMakeLists.txt` | build option | table既定ONを追加 | 低。portable既定不変 |
| `scripts/benchmark_engine_hotpaths.cpp` | `purchase_apply` | reachableな購入遷移256件だけの固定corpus micro | なし。benchmarkのみ |
| `scripts/run_phase0_baseline.py` | case registry | `purchase_apply`正式caseを追加 | なし |
| `scripts/benchmark_manifest.py`, `scripts/run_paired_benchmarks.py` | A/B contract | option記録と購入corpus correctness counter | なし |
| `tests/state_invariants_unit.cpp` | eligibility oracle | bonus 0..4の3,125組とeditor 255を独立規則で検証 | なし |
| `tests/test_engine_benchmark_tools.py`, `tests/CMakeLists.txt` | tooling/header contract | manifest軸、counter分類、standalone headerを検証 | なし |

`sizeof(PlayerState)=104`、`sizeof(Board)=392`、`sizeof(Game)=448`、layout digest
`210248ca4912bb94`はA/Bで一致した。public setter、binding、snapshot/editorは引き続き
`sync_packed()`を通り、範囲外bonusもclamp後のmaskが独立referenceと一致する。

## 24.4 correctness

| test | command | result |
|---|---|---|
| Release A | `ctest --test-dir /tmp/csplendor-phase2a1-off --output-on-failure -j2` | 33/33 pass |
| Release B | `ctest --test-dir /tmp/csplendor-phase2a1-on --output-on-failure -j2` | 33/33 pass |
| exhaustive eligibility | `state_invariants_unit` | 3,125 canonical組合せ + editor bonus 255、reference一致 |
| incremental differential | `incremental_hash_unit` | 1,000終局trajectory、全検査pass |
| separate-process differential | `validate_engine_differential.py --seeds 1000 --fixture-plies 32 ...` | 1,000 states / 12,000 semantic records一致、6,000 exact oracle、failure 0 |
| benchmark tools | `python -m pytest tests/test_engine_benchmark_tools.py -q` | 22 pass |
| Python full | `/tmp`のfresh Release extensionで`python -m pytest` | 556 pass、2 skip、4 deselect |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| ASan+UBSan | Debug、table=ON、hash verify=ON、`detect_leaks=0`でCTest | 33/33 pass、diagnostic 0 |
| TSan | Debug、table=ON、hash verify=ONでCTest | 33/33 pass、race/warning 0 |
| exact reveal digest | A/B、`five_moves`, depth 5, node limit 1,000,000 | node/order/reveal/digest一致 |

別process corpusはsetup action 31,998、legal action 23,617、digest
`cc630947b8455fa6b8fdeba587065e40eb80359149d58c62030cac1e84694bb8`。
購入microは256件全て`PURCHASE`で、各pairのpost exact/observable hash、score、turn、
winner、ordered legal metadataが一致した。

## 24.5 performance

Aはtable OFF、BはON。rateはops/sまたはsim/s、RSSはrunner peakの中央値。
全行22 pair、CIは11 crossover blockのbootstrap 95% CI。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| legal count | 1.182 M/s | 1.182 M/s | 1.0007 | [0.9978, 1.0025] | 5,680 | 5,534 |
| legal codes | 155,004/s | 154,910/s | 0.9997 | [0.9975, 1.0018] | 5,680 | 5,686 |
| random self-play apply | 1.597 M/s | 1.618 M/s | **1.0125** | **[1.0114, 1.0135]** | 5,708 | 5,688 |
| apply only | 20.075 M/s | 22.563 M/s | **1.1244** | **[1.1207, 1.1286]** | 7,646 | 7,642 |
| purchase apply | 13.077 M/s | 16.940 M/s | **1.2961** | **[1.2926, 1.2981]** | 7,982 | 7,974 |
| apply + exact hash | 17.161 M/s | 18.936 M/s | **1.1032** | **[1.0896, 1.1089]** | 7,790 | 7,764 |
| exact legacy 1T | 305,735/s | 304,763/s | 0.9961 | [0.9867, 1.0018] | 103,142 | 103,150 |
| Board copy/restore | 12.956 M/s | 12.977 M/s | 1.0018 | [0.9934, 1.0084] | 5,684 | 5,678 |

primaryの購入は+29.61%で10% gateを十分に超え、通常applyも+12.44%、
apply+exact hashも+10.32%。self-playも+1.25%でPhaseのend-to-end gateを満たした。
legal count/codesは+0.07%/-0.03%で1% regression gate内、Board copyとlegacy MCTSも
有意な悪化なし。RSSはprimaryで8 KiB減、Board layoutは不変。

hardware perf counter:

| metric | A | B | delta |
|---|---:|---:|---:|
| cycles | N/A | N/A | `perf_event_paranoid=4` |
| instructions | N/A | N/A | 同上 |
| IPC | N/A | N/A | 同上 |
| branch misses | N/A | N/A | 同上 |
| L1D misses | N/A | N/A | 同上 |
| LLC misses | N/A | N/A | 同上 |
| atomic RMW | unchanged | unchanged | tableにatomicなし |
| allocations | unchanged contract | unchanged contract | hot path追加なし |

正式runnerは`run_phase0_baseline.py`へA/B binary、8 `--case`、`--pairs 22
--warmups 3 --bootstrap-iterations 10000 --cpu-set 4`を指定した。展開済みcommand、
全pair比、slot inode/SHAはcompact evidenceへ保存した。

## 24.6 semantic equality

```text
node count: exact reveal 139,868 / legacy evaluated boards 65,536、A=B
legal moves: exact reveal 692,386 / fixture ordered legal count 250、A=B
TT hits/stores: exact reveal memo hits 17,163 / memoized states 67,615、A=B
action-order digest: edfd4c52f12a9c7a、A=B
reveal-order digest: 65bbe0573367e0a1、A=B
root visits: legacy 65,520、A=B
tree size: legacy 65,521、A=B
proof status: proven=false, reason=candidate_mate_not_verified, limit_reached=false、A=B
```

exact reveal digestは`9dd9c11919a4581a`、principal-line action/reveal digestは
`ee6f8466fd88410b` / `70ce0d9a743220ca`。legacy MCTS digestは
`0ab37fc767a05581`、formal expanded/action/inference/replay digestも全て一致した。

## 24.7 結論

```text
ACCEPT
```

購入micro +29.61%、通常apply +12.44%、self-play +1.25%でprimary gateを通過し、legal、copy、layout、
全oracle、sanitizer、探索量・順序・digestも維持したため採用する。

残存リスクは、immutable branchが購入ごとに1回残ること、hardware perf eventを取得できず
microarchitecture指標がないこと。branchは同一`.text`による厳密A/Bを優先した結果であり、
実測利益に含まれる。

commit hashは最終回答に記載する。次に実行すべき独立仮説は
**Phase 2A-2 — packed gems/bonusesの差分更新**のみとする。
