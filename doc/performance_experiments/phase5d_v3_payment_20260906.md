# 5D：V3 payment静的DPの試作・採否評価

## 結論

**REJECT_AND_REVERT**。静的composition DPを実装し、独立oracleと正式A/B・holdoutで検証した。
初案のcodec単体は約11倍、修正版の公開V3マスク生成は約1.13倍になったが、
V3自己対戦の速度低下が再現し、事前の回帰gateを満たさなかった。
DP表・codec置換・noinline指定・本番CMake切替を撤去した。追加高速化は採用していない。

残すものは公開V3経路の4 benchmark、native/Pythonの独立網羅oracle、圧縮試作・測定記録。
`src/`とルート`CMakeLists.txt`は開始時と差分なし。
4B-1、3D、3B sidecar、3C TT等の採用コード、48/V2/V3 schema、合法手列挙順・上限・
支払いルール・float順序は維持する。4Aは保留、5B-Rと4C三案の棄却も維持する。

## 基準と対象経路

- 開始: `59f56561eab14d4e912576c2b57beee04e62d25b`、source
  `99cabb84383dae5d757a6ff049a1c8541e751b32cf86f828f12d3fd0f1235323`。
- 共通benchmark基準: `3c3cb3e74d2efabbc0182cd1e32518a36100bd88`、source
  `b897b8856098bf65f70bc63267b27be0b44885e787e6bf7c2d02e63363b109cb`。
  このcommitのcodecは旧再帰方式。benchmark/manifest/事前計画だけを先に追加した。
  clean detached worktree `csplendor-v3-payment-baseline`を対照に使用。
- 作業branch: `perf/v3-payment-dp`。元`csplendor`のdirty source/HEAD/status、前Phaseの
  `csplendor-mcts-concurrency`は開始時と一致。reset/stash/forceやmainへの直接pushはしない。
- source/binary digestと全raw hashは[evidence](phase5d_evidence_20260906.json)へ保存。

実consumerは `csplendor/api/ai_manager.py` のV3 legal ID変換、3133手mask生成、
`decode_and_match`。`src/bindings_encoding.cpp`が公開codecへ接続している。
native48 MCTSはこのpayment codecを使わない。実NN・model・外部学習runnerはロードせず、
mask APIとV3を使う決定的ランダム自己対戦だけを測る。棋力/学習速度への主張はしない。

## 試作の内容

90枚×6位置×6合計値×uint16の**6,480 byte**のimmutable constexpr DPを実装した。
`ways[card][5][0]=1`、suffixからcost以下の値を足す方式。MAX_GOLD=5、カード枚数、
uint16上限（無制約compositionでも最大126）、全cardのpattern count/累積offset/合計2035を
compile-time assertionで検査した。

置換対象は `encode_payment_for_card` / `decode_payment_for_card` の再帰count呼出しのみ。
既存の入力検査、sum-prefix、graded lex順、invalid card/patternの返り値、公開再帰関数を保持。
動的初期化・allocation・可変cacheなし。旧2Bの「合法支払いをruntime filterする表」とは別物。

初案v1ではgeneric `ActionEncoderV3::encode`の生成コードが0x2eb→0x67a bytesに増えた。
一度だけv2としてcodec bodyのinlineを抑えた（GCC/Clang/MSVC向け、旧reference OFFは不変）。
v2では同symbolは0x2eb bytesに戻り、primaryは改善したが、自己対戦の回帰は残った。
これは生成コードサイズの観測であり、残った回帰の原因をcode layoutと断定したものではない。
hardware perfが利用不可のため、cache/branch費用の分解は未確認。

## 測定方法と採否gate

[事前計画](phase5d_plan_20260906.md)のprimaryは公開
`get_action_mask / gold_payment / 50,000 calls / simple_payment=false`。
このfixtureはcanonical editor設定であり、合法手84件中、非zero goldの購入が9件ある。
codec microは全2035patternを循環してencode/decode各100万回。
guardのV3自己対戦は固定seed42で10,000手、毎手mask→ID選択→decode_and_match→apply→子hash。
ランダム方策でありNNの方策ではない。初期化・terminal後のresetを含む。

既存native benchmark / paired runner / manifest / semantic digestを再利用。
portable Release、PERF/VERIFY OFF、CPU4、warmup2、22pairs/11 crossover blocks、bootstrap10,000。
倍率は2pair crossover blockのB/A throughput比をまとめた中央値（>1で改善）。
時間中央値の単純な比とは異なる場合がある。外れ値を削除せず、旧Phaseの倍率も乗算しない。
計時と重いbuild/testは並走させていない。

採用にはprimary **3%以上、95% block CI下端>1、独立holdout**を要求。
主要guardの2%超低下はCIと独立再測定で判定する。microだけの改善では採用しない。
測定後のprimary/閾値変更、最良runの選別は行っていない。

## 正式結果

時間は各側中央値。CIは95% crossover block CI。

| 試作 / slice | 正式 A→B (ms) | 正式倍率 [CI] | 独立倍率 [CI] |
| --- | --- | --- | --- |
| v1 mask primary | 138.148→133.404 | 1.0323 [1.0251, 1.0463] | 0.9785 [0.9683, 0.9869] |
| v1 payment encode | 91.983→7.668 | 11.3212 [9.5564, 12.2877] | 未実施 |
| v1 payment decode | 126.853→11.233 | 11.2926 [11.2663, 11.3558] | 未実施 |
| v1 V3自己対戦 | 15.560→16.907 | 0.9196 [0.9172, 0.9225] | 0.9161 [0.7519, 0.9213] |
| v2 mask primary | 100.035→88.982 | 1.1265 [1.1098, 1.1365] | 1.1352 [1.1271, 1.1445] |
| v2 V3自己対戦 | 15.666→16.972 | 0.9245 [0.9207, 0.9323] | 0.9583 [0.8902, 0.9972] |

v2はprimaryを通過した。しかしselfplayは正式・holdoutとも中央値で2%を超えて低下し、
両CIが1より下だった。holdoutは分散が大きく、CI全体が0.98より下という厳密な低下幅の
証明ではないが、今回の回帰gateを満たさない。追加variantや基準変更で救済せず棄却した。
v2 codec microはsmokeのみで、正式な約11倍という数値をv2へ転用しない。
広いfixture matrixはguard失敗で中止し、未実施をPASS扱いしない。

### メモリと診断

DPは6480 byteの静的表で、v1 diagnosticのcodec/mask計時中のallocationは0。
v2 primaryのnative RSS中央値は正式5,682→5,644 KiB、holdout5,644→5,648 KiB。
大きなRSS削減は確認されない。`rss_kind=current_resident_set`でありpeak RSSとは呼ばない。
runner RSSとは分けて保存した。diagnosticの時間を採否に用いていない。

codecの全pattern循環は高いgold支払いも多く含む一方、実局面では購入以外の手と低goldが
多い。micro倍率をmaskやゲーム全体へそのまま適用できない。

## 正しさと最終状態の検証

DP試作v1/v2それぞれについてRelease / recursive reference OFF / PERFの**各4 suites PASS**：

- 独立列挙oracleで、90枚×6^5 = **699,840入力**と全**2,035有効pattern**を照合。
  oracleはDP/再帰countを使わず、列挙後に(sum, lex)順へsortする。
- 全有効patternの各成分を0..255へ変更した**2,604,800組**、int card/pattern境界、
  無効値、全3133 action IDの逆変換、16 seed×最大128手のreachable mask/着手を確認。
- 既存encoding schema goldenによる48/V2/V3全ID、ルールquery、MCTS互換unitも確認。
- S0 pairedでは全runの結果digest、mask全byteの別digest、論理counterを一致。
  自己対戦は選択Action.packと子のexact hashを全手でdigestへ含める。
  v1のreference/candidate/PERF間も4sliceのsemantics/digestが一致した。

性能gate失敗後に本番試作を撤去し、独立oracleは汎用の`v3_payment_codec_unit`へ改名して保持。
以下は**撤去後の最終状態**の検証であり、DP試作のPython/sanitizer成功とは主張しない。

- Full native **40/40 PASS**。
- Python **568 passed / 1 skipped / 4 performance deselected**。この作業ツリーのextensionを
  importしたpathをrawへ記録。performance別実行は**4 passed**、package py_compileもPASS。
- 新規Python3テストは全pattern/offset、全ID、uint8境界、invalid値、vectorの短入力padding/
  長入力truncationという既存binding契約を検証。
- ASan＋UBSan **4/4 suites PASS**（codec/schema/rule/MCTS）。
  TSanは新しい共有可変stateがなく本番コードも撤去したため対象外。
- hardware perfは`perf_event_paranoid=4`で**N/A**、権限変更なし。

最終Release実行ファイルは共通基準・試作前harness・Python有効化後とSHA-256完全一致：
`c1b92751f3eaf1c9250dbce32102a6a6a2579522431436939f2ad73abaa7961e`。
元のengine source/CMakeも開始時と差分なし。改善しなかった試作を本番に残していない。
記録runnerのnonzeroはhardware perfの環境failureのみ。基準/候補のengineテストや
semantic gateのfailureは発生していない。

## 再現と次の候補

- [実行script](phase5d_record_20260906.py)、[集計audit](phase5d_finalize_20260906.py)、
  [evidence](phase5d_evidence_20260906.json)。全rawと試作は`doc/performance_experiments/raw/phase5d/`。
- 新しい記録先名で `CSPLENDOR_5D_VARIANT=再測定名 python .../phase5d_record_20260906.py build`
  を実行。現行本番にはDPがないため、このコマンドだけではDPを復活させない。
- 試作を再現する場合は共通基準`3c3cb3e`の別worktreeへ、`v1/prototype_sources.json.gz`または
  `v2/prototype_sources.json.gz`のtracked patchとnew_filesを復元する。source/binary digest付き。
  その基準には`CSPLENDOR_V3_PAYMENT_DP=OFF`のrecursive reference切替がある。
  現行本番のCMake切替は撤去済み。manifestのallowlistだけは過去試作の再現用に保持する。
- `CSPLENDOR_5D_BASE_SOURCE`で共通benchmark基準のworktreeを指定できる。
  現行scriptで試作を復元する場合、改名後の`tests/v3_payment_codec_unit.cpp`と
  `tests/test_v3_payment_codec.py`を持ち込まず、archiveの元の試作test名を使う。
- 保存済みrawは上書きしない。原依頼書は前Phaseの
  `raw/phase5br4b1/5br/v1/request.md.gz`を参照。新しい記録を`/tmp`へ残していない。

次の独立候補は**3E：visible-onlyのtake手child代表化**。実際の代表化費用の再確認を先に行う。
今回の5Dで停止し、3Eや保留中の4A、LTO/PGOには自動で進まない。
