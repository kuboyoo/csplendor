# 3E・5E・5A：着手選択の高速化・採否検証

## 結論

3件とも実装・採否評価を完了。**3E・5Eを採用、5Aの試作は棄却して撤去**した。
Phase完了と提案採用を区別する。3B/3C、3Dの採用経路・復元契約、4B-1は維持し、
4A見送り、5B-R・4C・5Dの棄却も変更していない。次のticketには自動で進まない。

| ticket / primary | 正式倍率 [95% block CI] | 独立再測定 | 判断 |
|---|---|---|---|
| 3E / visible_solver five_moves | 1.1057 [1.0970, 1.1249] | 1.1119 [1.1003, 1.1203] | 採用 |
| 3E / token_return (guard) | 1.1996 [1.1821, 1.2089] | 1.1923 [1.1885, 1.1993] | 通過 |
| 5E / full-action random selfplay | 1.0879 [1.0796, 1.0959] | 1.0786 [1.0575, 1.1023] | 採用 |
| 5A / decode_apply midgame_250 | 0.9695 [0.9475, 0.9857] | 0.9711 [0.9462, 0.9803] | 棄却・本番差分撤去 |

倍率は>1で速い。3Eと5Eは異なる経路の改善であり、倍率を掛け合わせない。
実NNの推論速度・棋力、詰め問題の保存速度、48手MCTS全体の高速化を示す値ではない。

## 基準と方法

- 開始基準: `9801610556419bf86ca71530592f643f76dc7ec0`、source
  `851bce178c48d332cfe0055e81861cac5789c8b04e53fa916d55d55e7fc9737e`。
  5D完了・DP棄却後の実ローカル成果物。公開3A参照commitへ戻していない。
- 5Eの独立基準は3E採用後 `2f2d8c8`、5Aは5E採用後 `b7ed7ad`。
  それぞれimmutableなdetached worktreeを用意。比較binaryのSHAもrawへ記録。
- 作業ブランチは `perf/action-selection`、worktreeは `csplendor-action-selection`。
  元 `csplendor` のdirty source/HEAD/statusと従来基準を保全。mainへ直接pushしない。
- [事前契約](phase3e5e5a_plan_20260906.md)と既存native benchmark / paired A/B / semantic digestを使用。
  portable Release、CPU4、warmup2、22 pairs / 11 ABBA crossover blocks、bootstrap 10,000。
  smokeは4 pairs。PERF・ASANと計時を分離し、重いbuild/testを計時と並走させていない。
- primaryは3Eで5%以上、5E/5Aで3%以上、CI下端>1と独立再測定を要求。
  関連guardで再現する2%超の回帰を認めない。旧Phaseの実測を転用していない。
- 固定予算solverと1-thread決定的MCTSはS0として意味・探索量を照合。
  非同期throughputのroot/tree一致を要求するテストは加えていない。NN/modelはロードしていない。

[manifest](phase3e5e5a_evidence_20260906.json)に全source/binary digest、圧縮raw、各runの
設定・絶対時間・RSS・失敗分類を保存。RSSはcurrent resident setでありpeak RSSではない。

### 最終組合せを開始基準へ直接比較

| slice | 最終版/9801610 [95% block CI] | 追加確認 |
|---|---|---|
| visible-only | 1.0879 [1.0810, 1.1058] | 100,000 nodes、合法手406,188件が一致 |
| full-action selfplay | 1.0720 [0.9871, 1.0854] | 独立再測定1.0789 [1.0742, 1.1007] |
| 1-thread native48 MCTS | 1.0027 [0.9876, 1.0190] | 高速化は主張しない |
| native48 decode/apply | 0.9733 [0.9624, 1.0063] | 再測定0.9638 [0.8536, 1.0552] |
| V3 selfplay | 0.9764 [0.8736, 1.0332] | 再測定1.0408 [0.9519, 1.0771] |

追加guardにはばらつきがあり、悪いrunも表・rawから除外しない。
decodeは同一harness・同一sourceで3E/5EをOFFにしたreferenceとも、予算を200万遷移へ
増やして比較した。その結果は **0.9867 [0.9793, 1.0061]**。
2%超の低下は確認されず、CIは1を含む。これは厳密な2%非劣性の証明ではなく、
すべてのmicro処理で速度不変という保証もしない。3E/5Eの事前primaryと関連guardを
採否の根拠とし、後から追加したmicroのノイズを高速化の根拠にはしていない。

## 3E：takeの同一結果を事前にまとめる

生成済み合法候補だけを扱い、TAKE_DIFFERENT/TAKE_SAMEの6色net deltaが同じものをまとめる。
購入・予約・貴族・PASSは混ぜず、最終child-key代表化とActionOrderKey sortはそのまま残す。
両takeの既存順序keyは `{2, 0, code}` なので、同じ群でcode最小を選ぶ。

初案はdeltaとcodeでsortするvector方式。正しさは通ったがsmoke 0.9211倍で棄却。
修正版は6色deltaを各3bitの**可逆な18bit値**として格納する1024-entry局所表を使用する。
ハッシュ値だけで同一化せず、6色分を欠損なく保存した値で完全一致を確認する。
候補数は最大 `10*56 + 5*21 = 665` なので表が満杯にはならない。
既存code vector内で圧縮し、追加heap確保なし、局所scratchは8192 bytes。
表の寿命は代表化呼出し内で終わり、探索木・Gameに持たせない。

所持8〜10個かつ銀行の各色7個以下の通常算術範囲に限定し、上限超過等はreferenceへ戻す。
候補生成以外の任意Actionを受け付ける公開APIではない。
全候補の適用snapshotをoracleとして、代表codeとchild群を比較した。
単体corpusでは重複候補11,406件を省略しても代表群は一致。
探索の枝刈り規則や探索量は変えていない。

## 5E：返却subtreeの件数でprefixを飛ばす

`legal_action_code_at`、`apply_legal_action_index`、`apply_random_action`を共通選択関数へ接続。
既存の小返却count/tableを利用し、先行base actionの返却subtreeを件数だけで飛ばす。
選ばれたbranch内のみ従来順で返却候補を列挙する。新しいpayment DPは追加していない。

二段階のMAX_MOVESと切捨てprefixを維持。選択indexは常に2048未満、base数も2048まで。
`random_value % count`と乱数消費は同一。noble/所持10個超は従来の完全列挙へ戻す。
token no-op、強制PASS、空候補、範囲外の0/false、履歴とundoを維持する。
一手分のActionと小さな局所変数だけを使い、局面cacheやmutableな共有表は追加しない。

新しい補助slice `legal_select` は件数を計時外で取得し、順位選択のみを測る。
同一harnessで新しい2フラグOFFをreferenceとしてtoken_returnを各100,000回比較：

| 計測対象 | 倍率 [95% block CI] | 解釈 |
|---|---|---|
| legal_count | 0.9872 [0.9763, 1.0244] | countアルゴリズムは変更なし |
| legal_select | 3.6782 [3.6273, 3.7784] | prefix materializationを削減 |

PERF版ではcount・selectとも計時中のheap allocationは0。
full selfplayはcountや局面更新も含むため、全体への効果は約1.08倍にとどまる。
simple paymentの正式selfplayは1.0784倍。再測定は1.1383倍だが分散が大きく、
その大きい値を代表的な改善幅として採用しない。

## 5A：source slot直接適用の棄却

同一encoder呼出し中で決定したvisible/reserved slotをGame内部へ渡し、既存の支払い・
カード取得・返却・貴族・終局・hash mutation処理を共有する試作を実装した。
descriptorの永続化やAction wire/schemaの変更はしていない。
public/editor向け試作はduplicate/cap時に旧source検索へfallback。
native48 adapterの経路へ接続し、hidden reservedのシフト・クリアも照合した。

Release、reference OFF、PERFの単体テストは通過したが、primaryは正式・再測定とも遅く、
採用条件未達。MCTS guardは正式1.0121倍、再測定1.0099倍で、primary失敗を救済しない。
Game/ActionEncoder/MCTS adapterの試作変更とCMake切替を全撤去した。
比較用フラグ名は過去rawを読むmanifest allowlistにだけ残す。
全48スロットのscan-reference oracleは既存native経路向けの回帰テストとして保持する。

## 正しさ・環境・再現

- 最終native **41/41 PASS**。
- Python **572 passed, 1 skipped, 4 deselected**、性能マーク **4 passed**、py_compile PASS。
- ASAN/UBSAN: selection、solver components、normal rollback、rule query、MCTS optimizationの5 suite PASS。
- 全合法手index **42,174件**。full/simple、base/final cap、範囲外、RNG、履歴/undoを比較。
- 全48 policy slot、隠し予約、重複card、editor cap、blank refill、終局/貴族、PASS policy外を照合。
- 本番拡張の実ロードパスを記録。Python有効化前後でnative計測binaryのSHAは一致。
  CMakeを使う通常のpackage buildでも3E/5Eは既定ON。
- 最初のテストに型不一致2件、oracleの範囲外前提1件、内部header分類の登録漏れ1件があり、
  すべて修正後に再実行。失敗rawも削除せず分類して保持した。
- hardware perfは `perf_event_paranoid=4` によりN/A。権限やシステム設定を変えていない。
  新しい並列処理はないためTSANは今回追加実行せず、既存native並列suiteは全体テストで確認した。

```bash
cmake -S . -B build/selection -DCMAKE_BUILD_TYPE=Release \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON
cmake --build build/selection -j4
ctest --test-dir build/selection --output-on-failure
```

参照版は `-DCSPLENDOR_GROUP_TAKE_CANDIDATES=OFF -DCSPLENDOR_RETURN_RANK_SELECTION=OFF`。
測定の正確なcommand、immutable binary SHA、working-tree source一覧はraw/manifestを正とする。
native Releaseの結果を実NNや問題生成の保存速度へ読み替えない。
