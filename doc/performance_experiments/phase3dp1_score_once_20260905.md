# Phase 3D-P1：めくれ候補スコアの一回計算

**ACCEPT（既定ON）**。3C後の同じ探索量で、代表deepは正式測定 **1.4966倍**、独立holdout **1.4860倍**。浅い探索は1.3072倍、warm sessionは1.5438倍だった。次のPhaseには進んでいない。

## 基準・変更範囲

- baseline：`35048e7`。R0で照合した `affb80a＋未コミット3C` を、新しい `perf/reveal-score-once` branchに内容同一で保存したcommit。開始HEADはR0記録取り込み後の`86b7473`。
- 基準source digest：`e715cdee085dd13d670b01f4571b23b1b7d47fc6868eb88cda08310013cd0d08`（R0の213ファイル）。元の `/tmp/csplendor-codex56-phase0` と依頼元repoの199ファイルは終了時もhash一致。元worktreeのユーザー差分は変更していない。
- candidate source digest：`d954909e798ac240727144270230e8dc0626ff3c3f85fbe8c68f32612fabb57a`。対象・個別hash・差分hashは[manifest](phase3dp1_evidence_20260905.json)。これはgit tree hashではない。
- 変更は `deck_reserve_cards()` / `order_visible_refill_cards_by_blank_probe()` のaction-local score計算だけ。score式、score降順→card ID昇順、候補集合、同型代表のfirst-seenを維持。rollback、TT、sidecar、数値演算順、global cacheは変更していない。

新しい内部helper `src/solver_reveal_order.h` は `{int score, int card_id}` を候補ごとに一度生成してsortする。通常候補は1つの `FixedStack<40>` 由来なので、40件・payload320Bの局所arrayを使う。40件超は境界確認後にvectorへfallbackする。候補0/1ではscoreを呼ばず、blank probe失敗時のearly returnもそのまま。返却色によってafter-gemsが異なるため、別actionへcacheを共有しない。

`CSPLENDOR_CACHE_REVEAL_SCORES=OFF` で旧comparator経路を残す。`CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON` では全候補のscore再照合と旧sort後の全ID列比較を実行し、不一致ならabortする。VERIFYはRelease採否計測には含めない。benchmark manifestもVERIFY ON/OFFの比較を拒否する（旧3Cに存在しないVERIFY optionはOFFとして正規化）。公開API/header契約は維持した。

## 正式paired A/B

Ryzen 9 7900X、GCC15.2、CMake4.2.3、Linux x86-64、portable Release `-O3 -DNDEBUG -std=c++17`。CPU4、各22pair/11 crossover blocks、2 warmups、10,000 bootstrap。既存 `run_paired_benchmarks.py` のfixed-inode slot crossoverを使用した。timing中にbuild/testは並走させていない。

AはR0と同じ3C採用binary（SHA `8a69dca8ce5a50fa6c5592f460a3730b98da4e1824fd6669685af46316c1f28e`）。Bは最終配置のRelease（SHA `126b720f9a067c679bfeea9e890822616a532f2824231c271df08b3f2f2782ab`）。Python module追加build後にもBのbyte一致を確認した。GNU textは960,518→953,698B（−6,820B）。OFF参考経路を含む診断buildの時間ではなく、実配置binaryの時間で採否を決めた。

倍率は**2pair crossover block比の中央値**、CIはblock単位bootstrap。下表のA/B rate中央値の単純比とは一致しない場合がある。全コマンド・sample・RSSは[CSV](phase3dp1_paired_20260905.csv)とmanifestが参照する `raw/phase3dp1/` に保存した。

| slice | A median nodes/s | B median nodes/s | paired倍率 | 95% CI |
|---|---:|---:|---:|---|
| **deep：hidden_reserve depth7 / 1M nodes** | 622,249 | 919,307 | **1.4966** | [1.4582, 1.5338] |
| shallow：five_moves depth3 | 590,566 | 770,772 | **1.3072** | [1.2939, 1.3121] |
| warm：five_moves depth7 / 500k nodes | 1,478,204 | 2,266,364 | **1.5438** | [1.5088, 1.5541] |
| visible five_moves / 100k nodes | 689,441 | 688,913 | 1.0014 | [0.9924, 1.0552] |
| visible forced_pass / 1M nodes | 2,265,999 | 2,270,420 | 0.9998 | [0.9846, 1.0094] |
| editor_fallback depth3 | 510,309 | 617,097 | 1.2082 | [1.2026, 1.2104] |
| hidden_reserve depth3 / attacker逆側 | 618,673 | 1,095,380 | 1.7783 | [1.7665, 1.7838] |
| reveal_heavy proof on（37 nodes） | 458,219 | 455,480 | 0.9956 | [0.9887, 1.0202] |
| reveal_heavy proof off（37 nodes） | 754,917 | 747,658 | 0.9832 | [0.9476, 1.0057] |

deepのmedian時間は1,607.096→1,087.778ms、peak RSSは54,058→53,998KiBでほぼ同じ。独立holdoutのdeepは **1.4860倍、CI [1.4678,1.5221]**。両方で事前の2〜3%採用目安を超えた。外れ値を削除していない。

`five_moves`は合法手数5のfixture名で、既知の5手詰み問題ではない。deep/warmは同じnode上限でUNKNOWNとなる固定仕事であり、「このdepth7局面が詰んだ」「探索可能手数が1.5倍」とは主張しない。warmは同予算のuntimed priming後、同じ部分cacheから開始する。計測はnative solve全体（後処理を含む）で、root順序検査は計時外。旧Phaseの改善倍率を掛け合わせていない。

### 小さいproof guardの測定限界と補完

約50µsのnative proof-offは追加22pairでも中央値0.9296、CI [0.8343,1.0307]と不安定だった。これは悪化なしの精密な証明ではなく、**native-only速度差は判定不能**として両結果を残す。コード・閾値を変更して良い結果を選んではいない。

補完として既存 `benchmark_solvers._reveal_fixture()` と `_sample()` を使い、2,000 solveをまとめたPython API測定を各22pair実施した（別の既存fixture、1 solve=33nodes）。proof-offは **0.9997、CI [0.9901,1.0257]**、proof-onは **0.9941、CI [0.9417,1.0732]**。毎回の結果dict・公開stats・全proof内容の一致も確認した。native proof-onと反復API proof-offに確認済み2%超の退行はなく、proof-onのAPI時間は依然CIが広い。

この補完時間はPython呼出し・native solve・結果変換・dict等値比較を含む。新timer/statisticsは作らず、ELF adapterから既存の計測を呼んだ。fixed slotはlauncherに適用し、Python/extensionのinodeは回転させていない。driverのbuild metadataと実extensionのhash/compile/link flagsは区別して保存した。これをnative primaryの改善率に混ぜない。

## 正しさと改善機序

正式全pairでresult/status/unknown、node/legal/terminal/memo等の公開counter、主手順、proof、root ordered action/outcome/reveal digestが一致した。VERIFY buildでもdeep/shallow/warm/visible/editor/proof/逆手番の全candidate score・ID列を検査。terminal/貴族待ちを含む追加fixture、gold/return/reserve limitについてnormal/simple paymentの両方を通した。

| 診断scope | 旧score呼出数 | 新score呼出数 | sort対象candidate数 |
|---|---:|---:|---:|
| deep visible refill | 147,699,302 | 12,895,182 | 12,895,182 |
| shallow visible refill | 192,682 | 17,646 | 17,646 |
| warm visible refill | 29,201,672 | 2,557,495 | 2,557,495 |
| 守備reserve frontier、返却6色×3level（各payment mode） | 5,592 | 468 | 468 |

逆手番benchmarkの計時範囲では守備reserve score呼出しは0だったため、その1.7783倍をreserve最適化の効果とは扱わない。専用frontier unitが18以上の異なるactionを生成し、独立score式でactionごとの順序を確認する。候補0/1/40/41/90、同点・負値・int最小最大、score例外伝播もunitで確認した。

deepのallocationは旧新とも **9,381,728回 / 累計1,303,606,263B**、clone442,934、Board snapshot1,442,934、restore1Mで同じ。今回削減したのはscoreの重複計算であり、copy/rollback/確保の削減ではない。約11.45倍のscore-call削減をエンジン11.45倍の予測に置き換えない。hardware cycles比率も主張しない。

| gate | 最終結果 |
|---|---|
| Release native full | **34 passed** |
| Python full、fresh deployment extension | **557 passed / 1 skipped / performance 4 deselected** |
| Python opt-in performance | **4 passed** |
| VERIFY fresh extension、詰み探索一式 | **29 passed**（既知5/7手、parallel、cache再利用/上限/再開、proof/frontier、cancelを含む） |
| ASan＋UBSan＋score/hash/sidecar VERIFY、native full | **34 passed** |
| benchmark tools / py_compile | **22 passed / PASS** |

初回のPython fullで、内部headerをpublic standalone matrixへ登録した候補固有の不整合1件を検出。public APIを増やさずinternal専用targetへ分離して修正し、full suiteを再実行した。CMake再生成漏れと補助adapterのprovenance/秒→ns変換の設定不備も修正済みで、失敗ログは削除していない。基準側のpaired実行failureは0。残存する候補test failureは0。

環境上の未実施は `generated/mate_puzzles2` 不在による任意テスト1件、hardware perf（R0でparanoid=4、権限変更なし）。実NN/MCTS速度・棋力は対象外。並列コードは変更しておらずTSanは再実行していない。

## 採否・再現

primaryは正式・独立holdoutで明瞭に改善し、shallow/warmも改善、全semantic gateとsanitizerを通過したため **ACCEPT**。小さいnative proof-offとPython proof-onの速度精度には上記の限界を残し、全fixtureで2%以内を数学的に保証したとは扱わない。

通常CMake buildでは既定ON。比較時は `-DCSPLENDOR_CACHE_REVEAL_SCORES=OFF`、oracleは `-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON`。採否の時間測定はVERIFY/PERF_INSTRUMENTATIONをOFFにする。[事前計画](phase3dp1_plan_20260905.md)、record/validateスクリプト、raw内の全実行引数と[manifest](phase3dp1_evidence_20260905.json)から再現できる。記録は全てdoc下、再生成可能なbuild成果だけがgit対象外directoryにある。
