# 5C-B：特徴量の定数表とPython NumPy境界

## 結論

5C-Bの実装・採否評価を完了した。**owning NumPy特徴量APIを採用し、定数表は棄却・撤去**。
既存consumer `StateFeaturizer.featurize` を実際に新APIへ接続した。
未使用の合法手code-buffer/fill API、外部dlsplendorのcanonical encoder変更は追加していない。
既にowning NumPyを返すActionEncoderのmask経路は `SKIP_ALREADY_DONE`。

| 案 / 計測対象 | 正式倍率 [95% block CI] | 独立再測定 | 判断 |
|---|---|---|---|
| 定数表 / native48 MCTS primary | 1.0229 [1.0091, 1.0488] | 0.9796 [0.9396, 1.0135] | 棄却 |
| 定数表 / state_encoder micro | 1.0503 [1.0330, 1.0591] | 未実施 | microだけでは採用しない |
| NumPy / StateFeaturizer primary | 12.5549 [11.2107, 13.0463] | 13.4260 [12.9348, 13.6068] | 採用 |
| NumPy / Python着手pipeline guard | 5.9723 [5.8654, 6.1123] | 5.9717 [5.8356, 6.3902] | 通過 |

NumPyの倍率は**Pythonの特徴量受け渡し経路**の値。実NN、学習、詰め問題保存、native MCTS
全体が同じ倍率になるという意味ではない。native engine benchmark binaryは開始基準と
SHA-256が完全一致し、ルール・solver・native MCTS本体には今回の本番差分がない。

## 基準・計測規約

- 基準commit: `1980b541bfde2db4d7f8fdba368b12013354881c` (3E/5E採用、5A棄却後)。
  source: `8efea08e588f8ddb3e05cbb4fef5c2bff3b7e64d98851904f3405687d2d4bbd9`。
- immutable baseline worktree: `csplendor-action-selection`。新branch: `perf/feature-table`。
  元のdirty `csplendor` と基準のHEAD/status/sourceは開始時と一致。reset/stash/force/main pushなし。
- [実施前契約](phase5cb_plan_20260906.md)に従い、二案を独立評価。
  primaryは中央値3%以上、CI下端>1、独立再測定で再現。他の主要workloadは2%回帰をguard。
- 既存paired runner / JSON schema / fixed-slot crossover / semantic digestを使用。
  portable Release、CPU4、warmup2、22 pairs/11 crossover blocks、bootstrap10,000。
  表案のsmokeのみ4 pairs。Pythonの修正後screeningは22 pairsで実施した。
  計時とbuild/full testsを並走させていない。PERF/ASANを採否時間へ混ぜない。
- 倍率は2-pair crossover blockのrate比の中央値。絶対時間中央値の比とは一致しない場合がある。
  悪いrunも削除せず保存し、歴史的倍率の掛算や有利なrunの選別をしていない。

### Python用の最小アダプタ

同じargvを要求する既存runnerを変更せず、基準/候補のrepoパスだけを焼き込んだ小さな
ELF launcherを用意した。launcherは同じPython benchmarkをexecするだけで、計時・統計は
行わない。計時はPython import、32局面の準備、warmup後に開始する。
**入れ替わるinodeはlauncherであり、ロードする.soそのものではない**。
実ロード先は各processでassertし、両.soのSHAも別途記録した。
同一.soで従来list経路と新consumerを比べた追加対照も **13.0839倍 [12.8468, 13.3538]**。
バイナリ配置差だけを改善根拠にしていない。

## 定数表：棄却した内容

90×8 floatとempty/hidden-tier行からなる3008-byte constexpr tableを試作。
元と同じ除算式で生成し、observer相手のhidden reserveはlevel以外を0にする。
全90card、空slot、observer -1/0/1、canonical swap、seeded reachable局面、
非正規card IDの例外まで、旧encoderと全196floatをmemcmpで照合した。
Release、table OFF reference、PERFの各oracle/MCTS suiteはPASS。

しかしnative MCTS primaryの正式値は3%未達、再測定でも改善が再現しなかった。
表・lookup置換・CMake切替を全撤去した。bitwise oracleは回帰テストとして保持。
`encode_public_card_statistics` のpool sortやfloat加算順、schema/versionは触っていない。

## NumPy境界：採用した内容

```python
features = csplendor.StateEncoder.encode_numpy(game, observer=0)
# dtype=float32, shape=(196,), C-contiguous, owning/writable

# 既存APIも同じ経路を利用。呼び出し側の変更は不要。
features = csplendor.StateFeaturizer().featurize(game, observer=0)
```

既存の `owning_array_copy` を再利用する8行のbinding追加とconsumer切替。
従来のC++配列→vector/list化→np.asarray変換を避け、C++配列から独立したNumPy配列へcopyする。
`StateEncoder.encode` / `encode_canonical` のlist返却は維持。
呼出しごとに別の所有配列を返し、stack scratchや次回searchで再利用される領域を公開しない。
配列の変更はGame・他の結果に影響せず、次回呼出し・Game破棄・search後も保持可能。
GILは従来どおり保持。浮動小数演算自体は変更せず、全byte一致を確認した。

out-bufferを受け付けないため、利用者がshape/dtype/容量を渡す必要はなく、
容量不足による部分書込みもない。invalid argument/cardの既存例外を維持し、失敗で
以前返した配列を書き換えない。observer -1は全情報、0/1は相手hidden予約を秘匿する。

### 実測範囲と絶対時間

primaryは32個のreachable局面を循環し、observer -1/0/1を切り替えて50,000回featurizeする。
戻り値の一要素をchecksumへ消費し、全196値の意味oracleは計時外。
guardは10,000手のfeaturize→同じrandom値で合法手選択/apply、終局後resetまで含む。
いずれも同じ処理量とfull-byte digestが全pairで一致。

| 経路 | 正式 A→B 時間中央値 | 再測定 A→B |
|---|---|---|
| featurize 50,000回 | 450.747→34.250 ms | 364.191→27.512 ms |
| Python pipeline 10,000手 | 80.361→13.295 ms | 82.352→13.371 ms |

Python側のRSSはrunner観測のみで、native側のcurrent RSSと混同しない。
メモリ削減率、実NNの速度/棋力向上は今回の測定から主張しない。
3E/5E等の採用経路を戻さず、今回の差分だけで開始基準に直接比較している。

## 検証と失敗の扱い

- native **42/42 PASS**、Python **581 passed, 1 skipped, 4 deselected**。
- Python性能マーク **4 PASS**、py_compile PASS。
- ASAN/UBSAN native **4 suites PASS**：feature bitwise、encoding schema、rule query、MCTS optimization。
  bindingの配列ownership/保持はPythonテストで検証。サニタイザ対象をPython拡張へ広げてはいない。
- 追加Pythonテスト9件：全bit一致、observer、owning/C-contiguous/float32/shape、
  非alias、90cardのhidden列、invalid入力、探索とGame破棄後の保持。
- 初回のPythonテストはpublic editorが不正card IDを先に拒否する前提を取り違えて1件失敗。
  setterの拒否も正しく検査するよう修正後、全テストを再実行した。
- 最初のPython A/Bはrepo引数差によりrunnerが計測前に拒否。runnerの検証を緩めず、
  固定launcherに修正。両失敗rawも削除せず保存。
- hardware perfはparanoid=4でN/A。権限・システム設定は変更せず、新しい並列実装もない。
  従来の並列native suiteは全体テストでPASS。実NN・外部repoの変更/検証は対象外。

[manifest](phase5cb_evidence_20260906.json)にsource/binary identity、全raw SHA、
採否gate、実行command、失敗分類を保存した。全記録はdoc下、生成binaryはbuild下で未追跡。
完了後は作業branchのみpushし、次のticketへ自動では進まない。
