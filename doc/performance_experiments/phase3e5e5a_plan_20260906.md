# 3E・5E・5A 実施前契約

基準は `9801610556419bf86ca71530592f643f76dc7ec0` (5D完了・提案棄却後)。
元の `csplendor` 作業ツリーは保全し、`perf/action-selection` で作業する。
依頼の順で独立に採否を決める。既採用最適化、4A見送り、5D棄却を変更しない。

| ticket | primary (既存native harness) | reference | 採用条件 |
|---|---|---|---|
| 3E | visible_solver / five_moves / 100000 nodes | 全候補を適用してchild代表化 | formal・独立再測定とも中央値1.05倍以上、block CI下限>1 |
| 5E | random_selfplay_apply / initial / 20000 transitions | count後prefix列挙 | 同1.03倍以上 |
| 5A | decode_apply / midgame_250 / 200000 transitions | decode_trusted + apply_trusted | 同1.03倍以上、MCTS guardも通過 |

既存paired runnerの4-pair smoke、22-pair ABBA (11 crossover blocks)、10000 bootstrap、warmup 2、CPU 4を使用。
不採用が明らかなsmokeではformalを省略できる。採用前には独立した22-pair再測定を必須とする。
回帰gateは既存の関連sliceで2%超の再現する退行を認めない。
3E: forced_pass・token_return・exact_reveal。5E: legal_count/codes、simple selfplay、editor。
5A: initial/gold_payment/hidden_reserve decode_apply、48 mask、1-thread parallel_scheduler。
全案の共通guardに固定予算solverと決定的MCTSを含める。数値演算順は変更しない。

3Eは生成済みtakeのみ、全delta列比較、同じ最小ActionOrderKey、最後のchild-key代表化を残す。
非正規/overflowはreferenceへ戻す。ordered代表codeと全snapshotを単体oracleで比較。
5Eは既存return count/tableでprefixを飛ばす。二段階MAX_MOVESと全index順、RNG、PASS、editorを維持。
5Aはslot情報を同一呼出し内だけ使用し、既存rule primitiveへ渡す。全48 slotと重複editorのoracleを置く。

計測用Release、reference、PERF診断、ASAN/UBSANは分離。非同期MCTSのtree完全一致は要求しない。
hardware perfは利用不可ならN/A、システム権限を変更しない。NN/modelは対象外。
試作が不採用なら差分をdocに保存して自身の本番変更を戻す。記録と再現oracleは保持可能。
採用版でnative/Python/関連ASAN、既存semantic digestを確認する。倍率は今回の実測のみ。
最終報告とmanifestをdocに保存し、日本語commitと作業ブランチへの通常pushで終了する。
