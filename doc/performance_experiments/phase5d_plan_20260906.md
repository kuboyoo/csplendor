# 5D 事前計画

開始基準59f56561eab14d4e912576c2b57beee04e62d25b、source
99cabb84383dae5d757a6ff049a1c8541e751b32cf86f828f12d3fd0f1235323。
4C三案は棄却済み。既存採用コード、元csplendorのdirty worktreeを保持。
branch perf/v3-payment-dp。4Aは保留、5B-R/4Cを再実装しない。

実consumer: csplendor/api/ai_manager.py のV3 legal ID変換、get_action_mask、
decode_and_match。src/bindings_encoding.cppから現行V3 codecを呼ぶ。
native48 MCTSには直接効果なし。NN/model/外部学習実験のロードや変更は行わない。

S0: printed costだけに依存する90×6×6 uint16 DP（6480 bytes）。
従来graded lex rank/unrankのcount_compositions呼出しだけ置換し、sum-prefixの順序、
無効card/pattern/uint8入力の返り値を維持。旧再帰関数とreference OFFを残す。
2B棄却の合法支払いruntime filter LUTとは別で、合法手列挙・上限・支払ルールは変えない。

primary: 公開V3 get_action_mask / gold_payment / 50,000 calls、simple_payment=false。
codec encode/decodeは全2035patternを循環して各1,000,000 calls。microだけで採用しない。
主要APIで3%以上、95% block CI下端>1、独立holdoutを要求する。
guard: V3 mask midgame_250/initial/token_return/editor_fallback/multi_noble/hidden_reserve、
公開V3 mask→decode_and_match→applyの固定seedランダム自己対戦、48 mask、V2/48全ID golden、
既存native決定的MCTS。主要guardの2%超低下はCIと独立再測定で判断。

既存paired/manifest/semantic digestを再利用、共通benchmarkを基準/候補に適用。
Release portable、PERF/VERIFY OFF、CPU4、22pairs/11 crossover blocks、warmup2、bootstrap10000。
固定量/全mask digest/選択actionと子hashを一致。時間の倍率と処理量の倍率を混ぜない。
PERF別buildはallocation診断用。本番数値演算順、LTO/native flagsを変更しない。

native oracle: 全699840個0..5入力、全2035有効pattern、uint8境界0..255を各成分へ、
無効card/pattern/int境界、全3133 action ID、全48/V2 golden、maskと実合法手対応。
Python bindingも全pattern/ID/無効入力を確認。採用候補のみfull native/Python/ASan+UBSan。
並列共有可変stateを導入しない。hardware perf不可はN/A、権限を変更しない。
性能基準を下げず、未達なら試作をdocへ保存して撤去。最後に日本語commitと作業branchへ通常push。
