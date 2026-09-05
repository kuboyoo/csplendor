# Post-3C R0：照合・限定再測定（2026-09-05）

R0を完了。**本番最適化は未実施**。次の候補は **3D-P1（めくれ候補スコアの一回計算）** 一件とし、ここで停止する。Phase 0の再構築、完了Phaseの再実装、棄却案の復活、checkout/reset/stash、pushは行っていない。

詳細・実行引数・個別source hash・全rawのSHA-256は [manifest](post3c_review_baseline_20260905.json)。計測は既存 `run_paired_benchmarks.py` / `benchmark_engine_hotpaths`、V3は既存 `benchmark_encoding.py` のfixture/timerを使用した。`doc/probes/` のstandalone PASSは本エンジンのgateに使用していない。

## 1. 実際のbaselineと保存先

| 対象 | HEAD / 状態 |
|---|---|
| **計測baseline** `/tmp/csplendor-codex56-phase0` | `affb80a1fac3d92dcbd0f7b3b0d480fca6cc4c52`、`perf/codex56-engine-hotpaths`。3Bまでcommit、**3Cは未コミット** |
| 作業依頼を受けたrepo | `a686973df58093e2e6c7d7bc83c8b2aedac1aa59`、`feature/opening-analysis-foundation`。こちらのHEADを3C baselineとは扱わない |
| 公開参照 | `0c5eba654ab4536c70947e725872cf5790db5e92`。3A後の記録保存commitであり、3Cコードを含まない |

baseline source digest（213対象ファイル）は `e715cdee085dd13d670b01f4571b23b1b7d47fc6868eb88cda08310013cd0d08`。対象は `src/csplendor/scripts/tests` のコード・設定と主要buildファイル。sorted `SHA256␠␠relative_path\n` 全体のSHA-256であり、git tree hashやバイナリhashではない。追加のtop-level `_build_support.py` は別hash・別archiveで保存した。

実内容を [source＋既存証跡archive](raw/r0_20260905/post3c_source_and_prior_evidence.tar.gz) に保存。全対象source、未追跡 `src/solver_tt_types.h`、0〜3Cの実在報告、3B/3C rawを含む。追加 [packaging helper](raw/r0_20260905/post3c_packaging_helper.tar.gz)、両repoのsource差分・全tracked差分、inventoryも同じdoc下に保存した。今後は空の隔離directoryへこの内容を展開して使用できる（HEADだけのworktreeでは代替不可）。生成バイナリ・モデルは保存していない。

開始時と終了時のsource全hash一致を確認。依頼元にあった `mate_frontier.py`、`bindings_solvers.cpp`、`test_reveal_verified_solver.py` の変更と未追跡依頼書は保持した。3Cを依頼元branchへ移植・commitする作業はR0には含めない。

## 2. 0〜3Cの採否照合

下表の判断は実在する旧報告と現行コード・compile definesの照合であり、旧実験の全再実行ではない。各報告の正確なpathはmanifest、内容は上記archiveにある。

| Phase / 案 | 判定・現状 |
|---|---|
| 0 計測・paired/digest基盤 | ACCEPT、既存を再利用 |
| 1A exact hash増分化 | ACCEPT、既定ON |
| 1B observable hash増分化 | REJECT、実装撤去済み |
| 2A 貴族mask / packed資源差分 | mask ACCEPT、資源差分 REJECT |
| 2B H1 single-pass codes / H2 return count | ともにACCEPT、既定ON |
| 2B H3 購入支払数runtime DP | REJECT、実装撤去済み |
| 2B H4a return table / H4b purchase table filter | H4a ACCEPT、H4b REJECT |
| 2B H5 packed code sink | ACCEPT、既定ON |
| 3A bounded path・card equivalence・reason/forced action圧縮 | ACCEPT。無制限minimaxのhash pathは残す |
| 3A 追加map単回lookup案 | REJECT、残存しない |
| 3B RevealSearchState sidecar | ACCEPT、`affb80a`、既定ON |
| 3C Stage 1 TT key/entry圧縮 | ACCEPT、未コミット、既定ON、`std::unordered_map`を維持 |
| 3C Stage 2 自前flat TT | NOT_IMPLEMENTED / NOT_ENTERED。Stage 1後のprofile開始条件未充足。実装後の棄却とは異なる |

3C報告のkey micro約−2.33%という既知の例外は保持する。過去のRSS改善等をR0再測定による改善率とは扱わず、旧Phase倍率も乗算しない。

新提案との重複：single-pass/direct code packing、増分reveal key、既存full Board＋sidecar RAII、compact TT/card class等の同等部分は **SKIP_ALREADY_DONE**。ただしfull-copy guardは新3D delta rollbackの完成を意味しない。3D-P1/P2、delta rollback、4Aのaccess epoch除去、5B-Rのscratch再利用、5DのV3 static codecは未実装。V3 codec案と棄却済みruntime購入手DPは別物として扱う。

## 3. 3Dへの接続契約

現行sizeof（bytes）は `Action=21 / PlayerState=104 / Board=392 / Game=448 / MoveList=43010`。

| 実際の状態・全field群 | 3Dで必要な復元・所有契約 |
|---|---|
| Board `bank[6]`, `visible[3][4]` | 変更前の値を復元 |
| `decks[3]: FixedStack<40>`, `nobles: FixedStack<12>` | 有効要素の**順序とcount**。任意めくれのerase→back移動はdeck countだけでは戻らない。未使用領域を論理状態と混同しない |
| `players[2]`: `gems[6]`, `bonuses[5]`, `points` | 現行referenceは両playerを含むfull Board。acting playerだけへ縮小するなら通常・特殊遷移の非変更性を別途検証 |
| player `packed_gems`, `packed_bonuses`, `noble_eligibility_mask` | authoritative配列だけでなく派生値・editor互換も保存 |
| player `reserved[3]`, `reserved_is_hidden[3]`, `reserved_count`, `purchased_count` | slot shift、hidden flag、countを全て戻す |
| player `purchased_cards`, `acquired_nobles` | provenance内容を維持。通常applyはappend-onlyなのでold sizeへのtruncate候補。ただし短縮・任意editor変更には適用不可 |
| Board `current_player`, `turn`, `final_round`, `waiting_noble`, `winner` | 全て保存。貴族待ちは通常の手番交代と異なる |
| Board `cached_hash`, `hash_valid` | validなhashとinvalid時の残存数値を区別。RuleMutatorの候補hashがcommit/破棄された**後**に外側guardが復元 |
| Game `board`, `history`, `board_history` | `clone_light()`はjournalなしの独立所有。solverのtrusted apply(false)はjournalを追加しない。public clone/undoのfull-copy契約は変更しない |
| Game `simple_payment_mode`, `blank_refill_mode` | simple modeはrootから固定。内部blank modeの一時変更を必ず復元 |
| sidecar `remaining_by_level_[3]`, `remaining_all_`, `acquired_hidden_`, `claimed_`, `rule_hash_`, `deck_order_hash_`, `active_` | Boardと同一scopeで全て復元。active/fallback状態も対象 |
| solver `root_`, `root_reveal_state_`, `hidden_catalog_.known_/hidden_` | root初期化時に構築、branch rollbackの変更対象にしない。主手順のscope終了でroot sidecarを戻す |
| memo / persistent TT / proof node map | branch rollbackでは巻き戻さない。stats、limit、generation/touchも探索全体の所有 |

3Bの所有者は `RevealVerifiedSolver::Impl`。`begin_search()`→input clone→blank mode→catalog初期化→`RevealSearchState::initialize()`→root保存。`apply_tracked()`は`observe_before()`（deck size/top、両player購入provenance長）→apply→`observe_after()`。任意outcomeの`move_deck_card_to_back()`はexact位置hashとsidecarのdeck hashも更新する。

canonical root検証はcard ID/levelの妥当性・重複なし・90枚保存、貴族保存、packed/mask整合、provenance/count/点/bonus、token総数・上限、予約slot等を確認する。非canonical editor入力、OFF flagは旧scan経路。想定外のdeck増減・購入provenance短縮・hash無効化等ではdeactivateしてfallbackし、VERIFY buildは不一致をabortする。simple payment modeを含む通常ルールと内部blank modeを扱い、公共Board ABIにsidecarを追加しない。

既存 `ScopedBranchRollback` はfull Board・sidecar・blank modeを保存し、**apply前**にmutatedを立て、反復restoreとdestructorで戻す。これはreferenceとして維持する。将来のdelta guardは二重復元を避け、apply=false・早期return・例外・cancelにも対応し、復元時の確保不要/noexceptを検証する。現行full-copy代入があらゆる異常入力でも確保不要と証明済み、とは扱わない。既存 `UndoRecord` は診断用で、任意deck並べ替えやeditor変更には不足する。proof/frontier出力の独立Game所有もscratch参照に置き換えない。

### TT・cache・上限

3Cの通常 `RevealStateKey=48 / DepthKey=56`、exact persistent用 `StateKey=32 / DepthKey=40`。前者はboard hash・unseen/acquired-hiddenの各128bit集合・両playerの点/購入/予約countや終局metadataを比較する。後者はroot-independent経路がzeroにするacquired-hiddenを省き、非zero入力はReleaseでも拒否。depthはkeyに含む。64bit board hashを含むkey全fieldのequalityであり、Board全内容について数学的に衝突しない保証ではない。

entryは通常24B、persistent32B（generation/touchを保持）、map valueは80/72B、proof value64B。visible key16B/depth24B、memo/force entry16B、bounds40B。unordered_mapのrehashはiteratorを無効化するが要素reference/pointerは維持する。再帰中にtrim/eraseしない現行規則を維持し、自前flat TTのlifetime規則を当てはめない。

cold solveはmemoをclear、warm `solve_reusing_exact_cache()`はexact TTを保持しroot制限を解除、exact/exhaustive・proofなしで再初期化する。generation wrapはclear。attackerはsolverインスタンスに固定。Python `MateSearchSession`はlockし、simple payment mode変更時にclearするが、native callerは自身でclearが必要。異mode・異attackerへ無条件に再利用できるcacheではない。

cache上限は**探索前後のtrim**で、探索中insertのhard cap/RSS上限ではない。reserve推定には2M上限とnode予算条件がある。touchに基づくtrimの同値時規則も維持する。clear/trim後もbucket capacityは残る。API内stats時間は後処理trimを含まないがnative benchmarkの外側計測には含む。node予算は毎node、cancel/timeは64nodeごと（0を含む）に確認。UNKNOWNを不詰みへ読み替えない。

referenceは3B OFFのscan、3C compact OFFの旧型、full Board rollbackとして残る（R0ではOFF全再測定をしていない）。visible-onlyはsolveごとに3TT clear・blank/deck除去し、bounded forced-winの線形pathと無制限minimaxのhash pathを分離。今回token cycleの最大path深さは108だったため、scratchを詰み深さ8で一律固定できない。

## 4. 実利用runnerと限定計測

`generate_mate_puzzles.py`→`puzzle_engine_adapter.py`→Python `PuzzleGenbuMCTS/GenbuMCTS`→legacy GenbuAdapter（PyTorch CPU）。隣接dlsplendor自己対戦もPython `MCTS`で、V3の3133手、`legal_actions`→個別encode、native V3 maskを使う。評価は直接PyTorch networkまたはinference client/Ray経由。隣接repoのdirty状態と参照ファイルhashもmanifestに記録した。

native legacy/shared/root-parallel APIは48手であり、上記主runnerからの使用は確認できない。以下native MCTSはライブラリ経路の代表測定で、実NNは使用していない。稼働中のlaunch設定・checkpointを特定できていないため、**実NN込みPython MCTSの1T/多worker性能はUNVERIFIED**。native synthetic値を問題生成・学習速度に換算しない。

環境：Ryzen 9 7900X（12C/24T）、Linux x86-64、GCC15.2、CMake4.2.3、Release `-O3 -DNDEBUG -std=c++17`、portable・採用flags ON。1T/solverはCPU4、8TはCPU4–11、governorはperformance。各5測定・2process warmup、seed42指定（fixture固定seedはraw参照）、MCTS20,000 simulation・batch16・synthetic latency0。native timing buildはinstrumentation OFF。バイナリSHAは3C採用版と同じ `8a69dca8ce5a50fa6c5592f460a3730b98da4e1824fd6669685af46316c1f28e`、GNU text=960,518B。

| 現行slice | 実処理数 | median時間 | rate/s | runner peak RSS KiB |
|---|---:|---:|---:|---:|
| exact five_moves depth3 | 1,397 nodes | 2.412 ms | 579,160 | 6,320 |
| exact hidden_reserve depth7 | 1,000,000 nodes | 1,665.766 ms | 600,324 | 54,016 |
| exact five_moves depth7 warm | 500,000 nodes | 356.236 ms | 1,403,564 | 33,752 |
| visible five_moves | 100,000 nodes | 144.271 ms | 693,141 | 11,256 |
| visible forced_pass cycle | 1,000,000 nodes | 439.365 ms | 2,276,012 | 13,660 |
| clone_light hidden_reserve | 1,000,000 copies | 30.011 ms | 33,321,210 | 5,728 |
| Board copy/restore hidden_reserve | 200,000 pairs | 13.818 ms | 14,474,032 | 5,728 |
| native legacy 1T exact midgame_250 | 20,000 sims | 49.939 ms | 400,489 | 34,740 |
| native root-parallel 1T determinized | 20,000 sims | 83.890 ms | 238,407 | 39,412 |
| native root-parallel 8T determinized | 20,000 sims | 20.808 ms | 961,165 | 39,492 |
| native shared/sharded 8T throughput determinized | 20,000 sims | 56.552 ms | 353,656 | 38,980 |

`five_moves`は合法手数5のfixture名で「既知5手詰み」ではない。shallowは指定深さで未証明・limitなし。deep/warm/visibleはnode上限によるUNKNOWNで、テストfailureでも永続不詰みでもない。warmは同予算のuntimed priming後の部分cache再利用であり、coldとの比を実装高速化率としない。MCTSの異backend/異threadは同じtree仕事ではない。root 1T→8Tのrate比は約4.03だが強さや探索深度の倍率ではなく、最適化A/Bでもない。

sharedの全5runでledger整合・virtual loss回収・reservation整合を検証。root訪問分布/tree digestの完全一致は要求していない。completed/s、unique evaluated boards/s、selected path steps/sをmanifestに保存。固定solver・同一構成の決定的MCTSはdigestを確認した。既存pairedのA/A確認は8pair/4blockのみ：B/A=0.9733、95% block-bootstrap CI=[0.9650,1.0163]。同一バイナリなので改善ではなく、2〜3%を単発測定だけで採用できないことを示す。

### 費用を示す診断（cycles比率ではない）

deepの同一100万nodeでallocation **9,381,728回 / 累計1,303,606,263B**、clone_light442,934回、Board snapshot1,442,934回、restore1,000,000回、temporary vector452,238回。累計確保bytesはlive RSSではない。sidecar key read594,195回・runtime fallback0、TT probe640,723回/hit141,958回。full deck scanは起きていない。

score回数だけを加えた隔離copyの診断patchをrawへ保存した（本番treeへ未適用）。releaseとの結果・順序digest・主手順・公開counterは一致した。

| score診断 | sort候補数 | score呼出数 | 候補あたり |
|---|---:|---:|---:|
| shallow visible refill | 17,646 | 192,682 | 10.92 |
| deep visible refill | 12,895,182 | 147,699,302 | 11.45 |
| warm visible refill | 2,557,495 | 29,201,672 | 11.42 |

このsliceでは守備側reserve score経路は0回で未カバー。deepのdeck-reserve候補4,900,198に対し実分岐204,981。全reveal候補17,795,380とdeck-reserve訪問だけを割らない。公開`reveal_branches=0`はvisible補充まで含む総訪問数ではなく、将来の全category `r_generated/r_visited`測定には補完が必要。

hidden_reserveは購入provenance空でclone allocation=0。そこで既存editor `reveal_heavy`も診断し、full/light cloneは各1allocation・3B、copy/restoreを含む100,000回では100,000allocations・300,000Bだった。空vectorのmicroを一般局面へ外挿しない。新しいscratch reset実装・性能は未検証。

fresh Python3.12 extension（同じ採用flags、pybind側はLTOあり）で既存16ply・合法手2のfixtureを測定：V3 mask **2.208M/s**、購入encode **4.877M/s**、合法手全件→V3 IDs **0.906M局面/s**、native48 mask **2.335M/s**。5×5,000回、mask/ID集合・decode roundtrip確認済み。多数合法手・全payment patternへの性能外挿は不可。CMakeの初期Python3.8自動選択は3.12明示指定で修正し、3.8生成物は測定に使っていない。

## 5. 確認結果・未確認・次の一件

native既存4テスト（solver components、state copy、deterministic parallel replay、scheduler limits）とfresh bindingのPython選択15テストがPASS。既知5/7手詰み、persistent再開・再利用・上限、proof、encodingを含む。既存reveal_heavy proof anchorも37node/36edgeでcomplete/validated、3C rawとdigest・公開counter一致（warmup回数だけ100対1000）。full suite/sanitizerを今回再実行したとは主張しない。

環境制限はhardware perfのみ：`perf_event_paranoid=4`、取得N/A、権限設定は変更なし。実行したbaselineテストのfailureは0。node予算UNKNOWNは別分類。候補固有failureは本番候補なしのためN/A。実NN throughput、reserve-score経路の効果、深い探索の完遂時間、全回帰・全matrixは未確認である。

**提案のみ：3D-P1**。`reveal_verified_solver.cpp` の `order_visible_refill_cards_by_blank_probe()` と `deck_reserve_cards()` の旧comparatorをreferenceとし、action-localな `{score, card_id}` を一度だけ計算する。score式、降順score→昇順ID、first-seen同型代表、返却色ごとのafter-gemsを変えない。rollback、TT、global cache、数値演算順の変更は混ぜない。

- primary：現行deep `hidden_reserve / depth7 / 1M nodes` の全体walltime。固定仕事で `time_A/time_B = rate_B/rate_A`。
- 性能gate：同一toolchain/flags/cache状態、既存22pair/11block以上のcrossover＋独立holdout。再現する約2〜3%以上・CIが1を跨がないことを基本とし、shallow/warm/visible/proof等の主要guardに確認済み2%超の退行なし。microのみ改善なら採用しない。
- semantic gate：候補ごとの旧新score・全ordered card ID、nodes/legal/terminal/TT hits、主手順、proof/frontierの意味一致。5/7手、editor fallback、候補0/1、blank probe失敗、終局/貴族待ち、守備reserve/返却色、cancel/limit/例外、cache再利用も確認する。未カバーのreserve経路fixtureを加える。

重複score約11倍は削減機会であって、エンジン11倍の予測ではない。**今回の実装高速化倍率はN/A。次チケットへ自動で進まない。**
