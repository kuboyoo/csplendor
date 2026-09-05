# Phase 3D-P2：再帰呼出しごとの一時配列再利用

**ACCEPT、既定ON**。採用済み3D-P1を基準に、代表deepは正式 **1.0848倍**、
独立holdout **1.0819倍**。浅い探索は1.0765倍、warm sessionは1.0731倍。
探索結果・探索量を維持し、全テストを通過した。3D-1には進んでいない。

## 基準と変更範囲

- baseline commit：`ff8b1f646b91ae094e47fc6898fff214f9e596ee`。
  source digest：`d954909e798ac240727144270230e8dc0626ff3c3f85fbe8c68f32612fabb57a`。
  3D-P1の最終manifestと一致する。公開旧3Aや元repoのHEADを基準にしていない。
- 専用worktree：`/home/kuboyu/workspace/repos/csplendor-search-scratch`、
  branch：`perf/reveal-search-scratch`。元repoのユーザー差分とP1 worktreeは変更せず、
  終了時にsource hash・status・HEADが開始時と一致することを確認した。
- candidate sourceの全対象・hash・digest・差分・実行引数は
  [manifest](phase3dp2_evidence_20260905.json)、全sampleは
  [CSV](phase3dp2_paired_20260905.csv)と `raw/phase3dp2/`。
  source digestはgit tree hashではない。

`SearchScratchFrame`にordered actionsとreveal IDsを所有させ、`clear()`後もcapacityを
保持する。frame自体は`vector<unique_ptr<Frame>>`の個別所有なので、外側vectorが伸びても
親のframe/vectorへの参照は動かない。RAII leaseの入れ子数で貸し出し、残り詰み深さで
添字や上限を決めない。通常手とめくれvisitorの入れ子は別frameになる。中断・例外・早期return
でもLIFOで返却する。solverコピーはTT等の既存状態をコピーし、scratchだけ独立した空の状態にする。

初期deque試作の4-pair smoke後、不要な初期確保を避ける個別所有方式に固定して正式測定した。
空のpoolはframeを確保せず、必要になった段だけ増える。inline Action配列は導入していない。
保持capacityはsolverの寿命まで残り、通常solve/TT clearとは独立する。全局面での絶対メモリ上限を
追加したわけではない（従来どおり探索上限・入力境界に従う）。

exhaustive attacker経路では、単にコピーして並べ替えていた配列を自分のframe内で並べ替える。
required root、strict prefix、既存bounded attacker filterの選別・順序は変えない。
filter経路の戻りvector、final-round/proof専用の局所action配列などは今回は維持した。
split_root/frontierが返すGame・edges、proof DAG、principal lineの所有権も従来どおり。
scratch参照を返却せず、外部からの保持・再生に影響しない。

3B sidecar、3C TT、rollback、上限/UNKNOWN/cancel、cache eviction、合法手・score式・
整数演算順は変更していない。P1のscore用40件/320B固定arrayは既にheap不要なので変更不要。
visible-only solver・MCTS・NN経路も変更していない。

## 正式paired A/B

Ryzen 9 7900X、GCC15.2、portable Release `-O3 -DNDEBUG -std=c++17`。
CPU4、各22pair/11 crossover blocks、2 warmups、bootstrap 10,000。
既存`run_paired_benchmarks.py`のfixed-inode rotationを使用。timingとbuild/testは並走しない。
VERIFY/PERF_INSTRUMENTATIONはOFF。倍率は2pair block比の中央値であり、rate中央値の比とは
厳密には一致しない。外れ値は削除していない。

| slice | A median nodes/s | B median nodes/s | paired倍率 | 95% CI |
|---|---:|---:|---:|---|
| deep：hidden_reserve depth7 / 1M nodes | 922,068 | 994,218 | **1.0848** | [1.0574, 1.0878] |
| shallow：five_moves depth3 | 751,036 | 814,068 | 1.0765 | [1.0035, 1.0900] |
| warm：five_moves depth7 / 500k nodes | 2,096,997 | 2,244,548 | 1.0731 | [1.0597, 1.0896] |
| visible five_moves / 100k nodes | 667,508 | 666,932 | 1.0130 | [0.9906, 1.0228] |
| visible forced_pass / 1M nodes | 2,236,112 | 2,226,585 | 1.0044 | [0.9942, 1.0064] |
| editor_fallback depth3 | 599,398 | 662,984 | 1.0960 | [1.0749, 1.1434] |
| hidden_reserve depth3 / attacker逆側 | 1,082,856 | 1,194,287 | 1.1050 | [1.0863, 1.1695] |

独立holdoutのdeepは **1.0819、CI [1.0783,1.0876]**。
正式median時間は1,084.519→1,005.823ms。RSSは48,982→49,158KiB（+176KiB、約0.36%）。
大きなRSS削減は主張しない。GNU textは953,698→956,366B（+2,668B）。
primaryは事前の5%目安を両seriesで超えた。

A binary SHA：`126b720f9a067c679bfeea9e890822616a532f2824231c271df08b3f2f2782ab`、
B：`ae3676e84de6f48951f69b44212926f60cd0e62ac1b36d21e1a1132c3e8b31a8`。
Python moduleを追加buildした後もBのbyte一致を確認した。

deep/warmは同じnode上限でUNKNOWNになる固定仕事であり、depth7の詰み完遂速度ではない。
`five_moves`は合法手5件のfixture名で、既知5手詰み問題とは別。既知5/7手は正しさのテストで確認した。
warmは同予算の計時外priming後のnative session再利用。過去Phase倍率の乗算はしていない。

### 小さいproof guard：単発と反復を区別

| 測定 | proof off倍率 [95% CI] | proof on倍率 [95% CI] |
|---|---|---|
| native単発・正式、37 nodes | 0.9800 [0.9608,1.0027] | 0.9758 [0.9551,0.9861] |
| native単発・独立holdout | 1.0091 [0.9766,1.0416] | 0.9708 [0.9660,0.9983] |
| native solve timerを2,000回集計 | 1.0086 [0.9901,1.0101] | 1.0243 [1.0144,1.0270] |
| Python APIを2,000回反復、別の既存33-node fixture | 1.0010 [0.9938,1.0182] | 1.0093 [1.0023,1.0121] |

単発proof-onでは約2〜3%低下を観測した事実を残す。CIは2%境界をまたいでおり、極小処理の
冷えた実行やcode layoutへの感度を排除できない。単発の悪化が絶対にないとは扱わない。

補完native adapterは既存`run_exact_reveal()`をそのまま呼び、**既存solve timerの値だけ**を
合計する。毎回fresh solver・同じwarmup・全digest/counter/semantics検査を行う。
fixture準備・結果照合・split_rootは従来どおり計時外。両側の静的core libraryはそれぞれ正式
配置buildとSHA一致。adapter自体の実行コード配置は正式binaryとは異なる。統計処理・paired
runnerは新設せず同じ22pair規約を使用した。反復native/APIでは2%超の退行が確認されず、
primaryの明瞭な改善と合わせ採用するが、単発測定の制約は残す。

Python補完はP1の既存`_sample()`・fixture・ELF launcherを再利用したもので、binding・結果変換・
dict比較込み。launcherだけをinode回転し、Python/extensionのinodeは回転しない。
native-only値と混ぜず、全build/hash/compile/link flagsを記録した。

## 確保削減と保持メモリ

以下は診断build。時間の採否には使っていない。

| slice | A allocation回数 | B allocation回数 | A 累計要求bytes | B 累計要求bytes |
|---|---:|---:|---:|---:|
| deep / 1M nodes、cold solver | 9,381,728 | 3,297,636 | 1,303,606,263 | 317,909,063 |
| warm / 500k nodes、priming後 | 2,584,328 | 1,499,870 | 192,959,633 | 25,946,249 |

deepの回数は約64.85%、累計要求量は約75.61%減少した。累計要求bytesはRSSでもlive bytesでもない。
Board snapshot/clone等は今回の対象外で、探索量とともに従来値を維持する。
既存`solver_temporary_vector_allocations`は論理的な一時配列使用箇所のcounterであり、実malloc数の
判定には使わず、global allocation counterで確認した。

| slice | 最大保持frame数 | frame内最大action capacity | reveal capacity | 検索境界での保持payload |
|---|---:|---:|---:|---:|
| deep | 24 | 474 | 64 | 60,880B |
| shallow | 11 | 256 | 64 | 29,552B |
| warm | 26 | 512 | 64 | 166,048B |
| editor fallback | 11 | 2,048 | 64 | 145,864B |

payloadは `frame数×48B + Σ(action capacity×40B + reveal capacity×4B) + 外側pointer capacity×8B`。
allocator管理領域を含まず、検索終了/priming終了時の最大保持量であり、探索中の瞬間peakではない。
全frameに一律上限容量を置いていない。仮に2048 wide Actionを各frameに固定配置すると、
26frameだけで2,129,920Bになる。無制限visible minimaxの108段を8段と扱う変更はしていない。

deepのordinary actionサイズ分布は0件=0、1〜16件=382,404、17件以上=69,833。
revealは0件=0、1〜16件=131,193、17件以上=516,720。editorではさらに大きなaction列がある。
入力依存の容量であり、ここから小さなinline Action容量を一律に決めなかった。

局所probeでfresh vector / capacity再利用 / fresh固定40-int arrayを比較した（6交互順batch、
checksum一致。エンジン正当性・採否の代用ではない）。16 IDsでは約81.95 / 42.60 / 31.05ns、
40 IDsでは193.95 / 138.55 / 124.88ns。固定array自体は速いが、証明された上限・型境界が必要で、
本番への追加効果はこのmicroから推定しない。P1採用済みscore arrayは維持し、可変長action/reveal
outputは今回capacity再利用に限定した。

## 正しさ・再現・停止点

| gate | 結果 |
|---|---|
| deployment native full | 35 passed |
| Python full、fresh extension | 557 passed / 1 skipped / performance 4 deselected |
| Python opt-in performance | 4 passed |
| score/hash/sidecar VERIFY、詰み探索Python | 29 passed（既知5/7手を含む） |
| ASan＋UBSan＋VERIFY native full | 35 passed |
| benchmark tooling / public header matrix | 23 passed |
| Python compile | PASS |

固定探索の全pairでstatus/UNKNOWN、node/legal/terminal/memo/persistent/保持TT数、主手順、
proof/frontier・root ordered action/reveal digestが一致した。診断ON/OFF/reference/VERIFYでも一致。
追加で貴族待ち・final round・予約上限・gold payment・token returnを両payment modeで検査した。
RAII unitは256段の入れ子、親参照保持、例外、中断後のwarm再利用、copy/assignment独立性を確認。
既存proof/frontier、cancel、cache上限・再開、並列詰み探索テストも維持した。

残存candidate failure、観測したbaseline failureはいずれも0。
環境未実施は任意の生成問題データ不在によるPython skip 1件とhardware perf（paranoid=4、権限変更なし）。
実NN/MCTS速度・棋力とTSan再実行は対象外。standalone probe PASSだけを採用根拠にしていない。

通常CMakeは`CSPLENDOR_REUSE_SEARCH_SCRATCH=ON`。`OFF`で旧局所vector経路を比較できる。
score/hash/sidecar VERIFYは正しさ検査に使用し、性能採否buildではOFFにする。
[事前計画](phase3dp2_plan_20260905.md)とrecord/setup/validate/supplementスクリプトから再現可能。
全rawはdoc下へ永続保存し、再生成可能なbuild/.soのみgit対象外に置いた。

次は計画の **3D-1：solver内部のtransactional rollback**。
今回それを実装していない。P2のframe寿命、3B sidecar、3C TT参照寿命・trim境界、既存full snapshot
referenceと上限契約を保持し、provenance vectorのdeep copy削減を独立に測る。
