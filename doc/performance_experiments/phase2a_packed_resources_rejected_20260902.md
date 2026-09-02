# Phase 2A-2: packed resources差分更新（棄却）

測定日: 2026-09-02

関連成果物:

- `phase2a_packed_resources_rejected_20260902.csv`: 正式8 workloadの22-pair集計
- `phase2a_packed_resources_rejected_evidence_20260902.json`: 全pair、固定slot、
  build identity、semantic recordを含むcompact evidence
- `/tmp/phase2a2-formal-mask-20260902.json`: 作業時raw artifact

## 24.1 状態

```text
Target phase: Phase 2A-2 — packed gems/bonusesの差分更新
Baseline commit: 59b29e223def149d5698e4c26d70528d7ed277f5
Branch: perf/codex56-engine-hotpaths
Final source state: 候補実装を完全revert。report/evidenceのみ追加
Compiler: g++ 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release, -O3 -DNDEBUG, C++17, portable, exact hash=ON,
             noble table=ON, instrumentation=OFF, hash verify=OFF
CPU affinity: CPU 4（SMT siblingなし、governor=performance）
```

正式A/Bは同一sourceから実験用
`CSPLENDOR_INCREMENTAL_PACKED_RESOURCES=OFF/ON`だけを変え、22 pair / 11
crossover block、各側3 warmup、10,000 bootstrap、ABBA、固定inode 2-slot
crossoverで測定した。build fingerprintは双方
`55447a1b7ffd0cd8ab442e81503c6f239d0f204383eed15b29cfaae4c26ab988`。

A binary SHA-256は
`a59bf50ede1562e169fae89b8e800c7c5a70eb59ad3b6f959e0544f0ef9da5a4`、
Bは`b0484e051b8d963d96cc066f44e368549e9b692601e5d8eec555bf42d376b172`。
AはPhase 2A-1採用バイナリとbyte単位で同一である。BのGNU `size` text categoryは
891,890から892,986 bytes（+1,096、+0.12%）、実`.text` sectionは
765,866から766,954 bytes（+1,088）となった。data/bssと公開型layoutは不変。

## 24.2 仮説

trusted transitionは既に各色のold/new値を扱っている。各12-bit laneをmask-clear/ORで
置換し、購入・返却後の5色再packを省けば、桁借り・桁上がりなしにapplyを短縮できると
仮定した。

次の形を順に試した。

1. OFF/ON両template経路を同一バイナリへ入れる厳密A/B形
2. 最終バイナリへ選択経路だけを入れるcompile-time形
3. known old/newのXOR lane差分
4. mutator commitまでgem差分を蓄積する形

1はtext categoryが906,554 bytesまで増え、直前採用版そのものより遅くなった。3は
text 893,382 bytesでexact hashがsmoke約-4%、4は895,818 bytesでexact hashとlegacy
MCTSが約-5%となったため早期棄却した。正式比較は最小コードだった2の
mask-clear/OR候補で実施した。

hardware counterは`perf_event_paranoid=4`のため取得不能だった。

## 24.3 変更

正式候補では次を実装して検証後、採否決定に従い全てrevertした。

| file | tested change | final state |
|---|---|---|
| `src/resource_bundle.h` | 12-bit laneのclear/OR置換primitive | revert |
| `src/board.h` | exact hash mutationと同じsetterでpacked値を差分更新、Debug array oracle | revert |
| `src/rule_transition.h` | trusted transitionだけ終了時の全再packを省略 | revert |
| `CMakeLists.txt` | 実験用OFF/ON compile option | revert |
| `scripts/benchmark_manifest.py` | 実験軸のallowlist/fingerprint除外 | revert |
| `tests/state_invariants_unit.cpp` | 5 lane × uint8全値の独立replacement oracle | revert |
| `tests/test_engine_benchmark_tools.py` | manifest軸contract | revert |

public setter、snapshot loader、raw solver transitionは候補中も従来の`sync_packed()`を
維持した。`sizeof(PlayerState)=104`、`sizeof(Board)=392`、`sizeof(Game)=448`、layout
digest `210248ca4912bb94`はA/B一致だった。

Phase 2A-3のbank/reserved/total-gems derived maskは、Board field追加前に要求される
end-to-end profile上の根拠がなく、今回の小さなcode-layout変化でも回帰が出たため追加しない。
必要性を示すprofileが得られるまでPhase 2B以降へ延期する。

## 24.4 correctness

| test | result |
|---|---|
| Release A CTest | 33/33 pass |
| Release B CTest | 33/33 pass |
| packed lane oracle | 5色 × new value 0..255、full packと一致 |
| incremental transition differential | `incremental_hash_unit` pass |
| benchmark tools | 22 pass |
| formal semantic contract | 全8 workload・各22 pairで一致 |
| revert後のPython全体 | 556 pass、2 skip、4 deselect |

候補は性能gateで棄却したため候補自体のsanitizer最終suiteは実施せず、実装を完全revert
した。revert後はPython全体を再実行した。またソースは、Phase 2A-1でASan/UBSan、TSanを
完走済みの採用状態と同一である。

## 24.5 performance

Aは差分更新OFF、BはON。rateはops/sまたはsim/s、全行22 pair。比と95% CIは11
crossover blockを統計単位とする。

| workload | A median | B median | B/A | 95% CI | 判定 |
|---|---:|---:|---:|---:|---|
| legal count | 1.146 M/s | 1.126 M/s | 0.9914 | [0.9731, 0.9987] | medianは-0.86% |
| legal codes | 153,788/s | 153,857/s | 1.0008 | [0.9739, 1.0025] | 維持 |
| random self-play apply | 1.537 M/s | 1.535 M/s | **1.0006** | [0.9968, 1.0020] | 1% gate未達 |
| apply only | 21.840 M/s | 21.580 M/s | **0.9879** | [0.9862, 0.9969] | -1.21% |
| purchase apply | 16.628 M/s | 17.079 M/s | **1.0292** | [1.0207, 1.0533] | 10% gate未達 |
| apply + exact hash | 18.338 M/s | 17.810 M/s | 0.9768 | [0.9634, 0.9869] | -2.32% |
| exact legacy 1T | 297,290/s | 300,226/s | 1.0153 | [1.0051, 1.0278] | +1.53% |
| Board copy/restore | 12.921 M/s | 12.598 M/s | 0.9969 | [0.9457, 1.0284] | 有意差なし |

primaryの購入改善は+2.92%に留まり、通常applyは悪化、self-playは+0.06%だった。
したがって「purchase/apply 10%以上、またはself-play 1〜3%以上」を満たさない。
primary RSS中央値はpurchaseで+34 KiB、self-playで-32 KiBであり、実用上のRSS改善も
確認できなかった。

## 24.6 semantic equality

```text
fixture ordered legal count: 250、A=B
fixture ordered legal digest: f05af3b244e08b82、A=B
purchase corpus: 256 transitions、post exact/recomputed hash一致、A=B
random self-play final exact hash: 1011940230593276800、A=B
legacy evaluated boards: 65,536、A=B
legacy root visits/tree size: 65,520 / 65,521、A=B
legacy digest: 0ab37fc767a05581、A=B
formal expanded/action/inference/replay digest: 全てA=B
Board restore: restored_exactly=true、A=B
```

全workloadでcorrectness digest、ordered legal metadata、score/current player/turn/winner、
exact/observable hashが一致した。意味論の不一致は0件。

## 24.7 結論

```text
REJECT — 実装差分は完全revert
```

局所的な購入改善は再現したがPhase gateには届かず、通常applyとexact-hash hot pathの
回帰を相殺できない。Phase 2Aとして採用する実装はcommit `59b29e2`の貴族適格mask
table化のみとし、packed差分更新とprofile根拠のないBoard derived maskは導入しない。
