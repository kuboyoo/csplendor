# Phase 3D-1：通常着手のtransactional rollback

採用範囲は **visible-only solverの通常着手**。3D-P2比で正式 **1.1824倍**、独立holdout
**1.1725倍**。めくれ込みsolverまで適用したv1/v2はproof回帰gateを満たさず棄却し、
最終候補v3には含めない。Phaseの検討完了と、試した全提案の採用を区別する。

## 基準・実装境界

- baseline commit：`ef312a15f451d566c428ce7b955a788ede6c533c`（採用済み3D-P2）。
  source digest：`8c402dfd467ec18944c0541eb2ec3191e0ba8a99a393818b7d0895221d983611`。
- 作業branch/worktree：`perf/solver-normal-rollback` / `csplendor-normal-rollback`。
  元repoのユーザー差分と3D-P2 worktreeは、開始・終了時のsource hash/status/HEAD一致を確認した。
  旧3AのHEADへ戻した計測ではない。
- 全sample・採否・source/binary hashは [manifest](phase3d1_evidence_20260905.json)、
  [CSV](phase3d1_paired_20260905.csv)、`raw/phase3d1/`。最終版はその中の `candidate_v3/`。
  v1/v2の結果を最終版の速度として集計しない。source digestはgit tree hashとは別物。
  最終source digest：`aff9b0e8407df4309401957f0c79455cddf86582d6384a159ee512d4f75a01dc`。
  rawは307件を圧縮保存し、manifest.tsvに圧縮前後のSHA-256を記録した。

既存の内部`UndoRecord`を再利用し、`NormalBranchRollback`が非vectorのrule field、
provenanceのold size、必要なdeck top、Game modeを保持する。通常Game遷移が購入・貴族の
provenanceをappendするだけという契約を利用し、復元時は末尾をtruncateする。
公開Game::undo/historyはfull Boardのまま。Action表現、合法手順、評価式、詰みの定義、
上限、UNKNOWN、TT容量・eviction、P2 scratch、MCTS/NNは変更しない。

visible-onlyの代表手生成・bounded forced search・minimaxの3か所へRAIIを適用。
初回にEditor invariantを検査し、不適合なら同じguardのfull Board経路へfallbackする。
入力そのものを正規化したり、非法なeditor入力を安全と認定したりはしない。

`src/reveal_verified_solver.cpp` は3D-P2とbyte一致。Releaseの同translation unitも
SHA-256 `14ffd1d781429306dee473acbba51c99a05f75b920fe914b0526bd66df811d2e` で一致する。
3Bのfull Board＋sidecar guard、oracle、任意位置めくれ、proof/frontier、root列挙を維持した。
これはlinked binary全体の速度が完全一致するという意味ではなく、別途A/Bで回帰を確認する。

## 復元・例外契約

全field表とacting playerのみの更新を確認したcall graphは [事前計画](phase3d1_plan_20260905.md)。
parent/childの比較にはserializationでなく、両playerの全rule field、provenance内容、
active deck/nobleの順、packed値・noble mask、hash_validとraw cached_hashを使う。
vector capacity・padding・inactive deck末尾は意味論に含めない。

guardはapplyの**前**にmutation済みフラグを立て、false、throw、早期returnでも戻す。
RuleMutatorはGame apply内で先に寿命が終わるため、復元後に古いhash candidateをpublishしない。
restoreはnoexcept・確保不要。append-only違反はabortし、REFUTEDへ変換しない。
full fallbackもvector capacityを先に検査し、copy assignmentで確保しないことを保証する。

通常手は高々1山のtopをpopするが、入れ子の子がinactive slotを書き換え得るので、
共通guardは各山のold topも戻す。countだけを戻す設計にはしていない。
ただし任意位置erase/rotateの復元にこのguard単体を使ってはいけない。
最終採用のvisible-onlyでは山は空であり、このtop契約はunitで検証した将来接続用。

`UndoRecord`は128B、Release guardは416B（fallback用variantを含む）、VERIFY guardは824B。
小さくなったのは主にdeep copyとheap確保であり、stack上のguardが128Bになったとは主張しない。
3B sidecarを新guardへ移す案は棄却し、既存guardに所有させたままなので二重復元しない。

## 最終候補v3のpaired A/B

Ryzen 9 7900X、GCC15.2、portable Release `-O3 -DNDEBUG -std=c++17`。
VERIFY/PERF OFF、CPU4、各22pairs/11 crossover blocks、2warmups、bootstrap10,000。
既存runnerのfixed-inode rotationを利用し、計時中はbuild/testを並走させない。
倍率は2pair block比の中央値。外れ値は除去せず、rate中央値の比とは区別する。

| slice | A median nodes/s | B median nodes/s | paired倍率 | 95% CI |
|---|---:|---:|---:|---|
| visible five_moves / 100k nodes：primary | 677,359 | 770,015 | **1.1824** | [1.1685,1.1879] |
| 同・独立holdout | 695,403 | 819,960 | **1.1725** | [1.1596,1.1946] |
| visible forced_pass / 1M nodes | 2,197,227 | 2,567,668 | 1.1741 | [1.1593,1.1757] |
| exact hidden_reserve depth7 / 1M nodes | 993,129 | 992,340 | 1.0001 | [0.9952,1.0028] |
| exact five_moves depth3 | 825,236 | 819,817 | 0.9942 | [0.9846,1.0020] |
| exact five_moves depth7 / warm / 500k | 2,200,351 | 2,193,474 | 1.0064 | [0.9988,1.0175] |
| exact editor_fallback depth3 | 660,764 | 662,614 | 0.9998 | [0.9975,1.0088] |
| exact hidden_reserve depth3 / attacker逆側 | 1,204,358 | 1,203,231 | 1.0009 | [0.9937,1.0098] |

deepの独立holdoutは0.9924倍、CI [0.9854,1.0072]。最終版のexact/warm高速化は主張しない。
deep/warmはnode上限でUNKNOWNになる固定仕事で、7手詰み完遂速度ではない。
`five_moves`は合法手5件のfixture名で、既知5手詰み問題とは別。既知5/7手は正当性テストで扱う。
過去Phaseとの改善倍率の乗算は行わない。

visible primaryのnative RSS中央値は11,254→11,246KiBで、RSS削減の主張はしない。
deepは49,152→49,196KiB。GNU textは956,366→963,880B（+7,514B）、dataは4,080→4,184B。
variantのfallbackや内部oracleを含むコード増加を隠さず記録する。

### 短いproofの回帰判定

| 最終v3の測定 | proof off倍率 [95% CI] | proof on倍率 [95% CI] |
|---|---|---|
| native単発・正式、37 nodes | 0.9871 [0.9691,1.0538] | 0.9858 [0.9726,1.0308] |
| native単発・独立holdout | 1.0047 [0.9657,1.0872] | 1.0104 [0.9615,1.0469] |
| native timer 2,000回集約・22pairs | 0.9941 [0.8415,1.0003] | 0.9879 [0.9811,1.0089] |
| 同・独立66pairs/33blocks | **1.0016 [0.9988,1.0055]** | **0.9940 [0.9908,1.0020]** |

極小の単発計時と初回集約にはばらつきがあり、独立66pairsを事前固定して補完した。
最終gateで両CI下端が0.98以上。単発の悪化が全くないという主張はしない。
既存P2 adapterが毎回fresh solverを作り、既存solve timerだけを合計する。
各callのdigest/counter/semanticsを照合し、両側core static libraryは正式buildとSHA一致する。
adapterのcode layoutは正式binaryと異なる。この補完と単発の結果は混ぜない。

### 棄却案と採用版を分ける

| variant | 範囲 | 判断 |
|---|---|---|
| v1 | visible＋reveal通常手、proof/rootにも適用 | 棄却：独立native proof-on 0.98081、CI下端0.97775 |
| v2 | visible＋reveal再帰本体のみ、proof/rootは旧方式 | 棄却：独立native proof-on 0.98210、CI下端0.97792 |
| v3 | visibleのみ、revealソースとobjectは3D-P2維持 | ACCEPT、既定ON |

2%の判定基準を緩めてv1/v2を採用しない。初期shallowに0.907倍が出た記録、
その独立単発1.015倍/既存native集約1.013倍を含め、全rawを保存する。
v1/v2のwarm約1.05倍とexactの確保削減も最終版の成果に数えない。
この37-node proof fixtureではcompact guardの実行数は0だったので、低下原因を
compact restore命令そのものと断定しない。コード配置等の間接影響は未特定である。

## 診断・正しさ

診断buildの以下の時間は採否に使わず、確保削減の機構確認に使った。

| slice | A allocation回数 | B allocation回数 | A累計要求bytes | B累計要求bytes |
|---|---:|---:|---:|---:|
| visible / 100k nodes | 2,554,415 | 805,487 | 62,590,026 | 58,480,929 |
| visible forced_pass / 1M nodes | 7,175,922 | 2,299,459 | 191,715,669 | 150,692,261 |
| exact deep / 1M nodes | 3,297,636 | 3,297,636 | 317,909,063 | 317,909,063 |
| exact warm / 500k nodes | 1,499,870 | 1,499,870 | 25,946,249 | 25,946,249 |

visible primaryの確保回数は**68.47%減**、Board snapshot数は876,180→2、
compact restoreは876,178回。clone_lightは2回のまま。forced_passはGame内部PASSの
既存snapshotを残すため3,231,559→3,170。累計要求bytesはRSS/live bytesではない。
visibleのguard強制full referenceとcompact/VERIFYで全digest・探索量が一致した。
exactは最終版では対象外で、allocation・snapshot・clone数も変更していない。

| 最終gate | 結果 |
|---|---|
| native full | 38 passed |
| Python full、fresh extension | 557 passed / 1 skipped / performance 4 deselected |
| Python opt-in performance | 4 passed |
| rollback/score/hash/sidecar VERIFY、詰み探索Python | 29 passed（既知5/7手を含む） |
| ASan＋UBSan＋VERIFY native full | 38 passed |
| benchmark tooling / public header matrix | 23 passed |
| package・記録スクリプトpy_compile | PASS |

新unitは3,016遷移＋特殊fixture、全7 ActionType、normal/simple payment、blank/refill、
valid/invalid hashを照合する。予約slotの先頭/中間/末尾shift、貴族自動獲得/選択待ち、
final round、0/1枚山、入れ子、inactive top書換え、restore無確保・冪等性、mode/historyを確認。
実際の支払い5色途中、provenance append、source除去、貴族処理、hash commit前後に注入し、
visitor先頭/中間/最後のreturn/throw、部分支払いfalse、node limit、途中cancel後の再開を検証。
実visible/reveal solver双方で例外がREFUTEDへ変換されず、同instanceを再利用できることも確認した。

全固定探索pairのstatus/UNKNOWN、nodes/legal/terminal/memo/persistent/保持TT、主手順、
proof/frontier、ordered action/reveal digestが一致。追加fixtureでは貴族・最終round・予約上限・
gold payment・token returnを両payment modeで検査した。既存並列探索テストも維持したが、
非同期MCTSの訪問分布一致を本件の採否条件にはしていない。

初回VERIFY unitの人工30-token fixtureが合法手上限を先に埋めた問題と、Python extensionの
新依存`state_invariants.cpp`不足によるimport failureを修正し、元の失敗ログも保存した。
後者はcandidate固有の実装漏れであり、環境起因とは扱わない。最終版の残存failureは0。
baseline failureは観測なし。環境未実施は生成問題データ不在によるoptional skip1件と
hardware perf（paranoid=4、実行拒否、N/A）。権限変更は行っていない。TSan/MCTS/NN速度は対象外。

Python module追加buildおよび最後のテスト追加後も計時binaryのbyte一致を確認した。
A binary SHAは `ae3676e84de6f48951f69b44212926f60cd0e62ac1b36d21e1a1132c3e8b31a8`、
Bは `d575cb94408686fa7923ae48e651e234e116f5101c84cb476a85e9b5a4de1ff6`。

## 再現と停止点

通常buildは `CSPLENDOR_SOLVER_NORMAL_ROLLBACK=ON`、`OFF`でvisibleのfull snapshot
RAII referenceへ切替可能。正式baselineはOFF試作ではなく3D-P2の未変更binaryを使用した。
`CSPLENDOR_VERIFY_SOLVER_ROLLBACK=ON` はchild/parent oracleと故障注入を有効にする。
VERIFY時だけRuleMutator::commitのnoexceptを外し、前後へ例外を注入可能にする。
Releaseでは従来のnoexceptを維持し、故障注入hook/TLSは存在しない。

record/setup/validate/native_batchスクリプトは既存計測・統計のorchestratorであり、計測基盤の
作り直しではない。最終版の再現時は `CSPLENDOR_3D1_VARIANT=candidate_v3` を指定する。
rawは上書き禁止のため再実行先は分ける。buildとextensionのみgit対象外で、記録は全てdocに保存。

次は3D-2の任意位置deck復元を独立に検討できるが、今回は開始しない。
そのbaselineのreveal経路はfull Board＋sidecar、TT/scratchの寿命も3D-P2のまま。
棄却v1/v2を無条件に復活させず、触る山のactive内容保存と通常手guardの接続を再評価し、
proofを含むE2E gateを満たすことが必要。4A-1も本件では開始していない。
