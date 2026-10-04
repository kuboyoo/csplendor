# 速度ベンチマークの測定条件と出典

[README](../README.md#速度ベンチマーク) の表は保存済み測定の抜粋です。この文書は利用者が処理能力を見積もるために、測定対象と適用範囲をまとめます。開発フェーズごとの採否判断は原記録へ分離しています。

## 2026-10-04: V3探索・行動符号化・snapshot

比較元はmain `0088f76107a54a50b0f366277c3ecc0aa9f1a6aa`、高速化後はマージcommit `c35062d`（ソースは `fee295b` と同一で、lint修正のみの差）。探索結果・特徴量・行動IDは比較元とビット一致します（V3探索は全バッチの特徴量・合法手ID・根の訪問数と行動価値のダイジェストで確認）。

| 条件 | 値 |
|---|---|
| CPU / OS | Ryzen 9 7900X / Linux x86_64 |
| ツール | GCC 15.2、Python 3.12.1 |
| ビルド | portable Release（既定のCMakeオプション） |
| 負荷 | 別の学習ジョブ（CPU 約4コア相当）が並走。C++ベンチはCPU1コアに固定 |
| 集計 | C++ベンチは base/候補を交互に7回実行した中央値。Python計測は交互3回の中央値 |

### V3SearchSession（Pythonから呼出し、NN推論を含まない）

48局×400探索、葉batch 32。priorとvalueは葉の内容から決める決定的な擬似ネットワークで与えています。

| 構成 | 比較元 | 高速化後 | 速度比 |
|---|---:|---:|---:|
| 決定化あり（既定） | 111,601 sim/s | 157,477 sim/s | 1.41 |
| 決定化あり＋二段階選択 | 113,358 sim/s | 161,841 sim/s | 1.43 |
| 決定化なし | 116,567 sim/s | 152,908 sim/s | 1.31 |
| root rollout あり | 161,603 sim/s | 228,916 sim/s | 1.42 |

`num_threads` によるスケーリング（64局×400探索）:

| スレッド数 | sim/s | 比較元（逐次）比 |
|---:|---:|---:|
| 比較元（逐次） | 99,574 | 1.00 |
| 1 | 162,564 | 1.63 |
| 2 | 273,959 | 2.75 |
| 4 | 464,945 | 4.67 |
| 8 | 716,536 | 7.20 |
| 16 | 798,148 | 8.02 |

16スレッドでの伸びの鈍化には、並走ジョブとのコア共有の影響を含みます。実モデル推論を含む自己対局全体の倍率ではありません。

### C++内部処理（`scripts/benchmark_engine_hotpaths.cpp`、1回あたりの時間）

| 処理 | fixture | 比較元 | 高速化後 | 速度比 |
|---|---|---:|---:|---:|
| 合法手count | initial | 364.1 ns | 197.5 ns | 1.84 |
| 合法手count | midgame_250 | 333.8 ns | 326.0 ns | 1.02 |
| 合法手1件の選択 | initial | 186.2 ns | 98.0 ns | 1.90 |
| ランダム自己対局1手 | midgame_250 | 370.4 ns | 323.9 ns | 1.14 |
| V3合法手マスク | midgame_250 | 3,616.0 ns | 2,691.8 ns | 1.34 |
| V3自己対局1手 | midgame_250 | 1,523.0 ns | 1,213.7 ns | 1.25 |
| V3支払いencode | midgame_250 | 64.6 ns | 7.1 ns | 9.1 |
| V3支払いdecode | midgame_250 | 127.1 ns | 11.5 ns | 11.1 |
| 可視限定ソルバ | reveal_heavy | 3,737.0 ns | 3,466.5 ns | 1.08 |

厳密めくれ探索・従来型MCTS・共有tree・`state_encoder`・`apply` は ±1% 以内で、変化はありませんでした。

### Python API

| 処理 | 比較元 | 高速化後 | 速度比 |
|---|---:|---:|---:|
| `Game.deserialize_snapshot`（184バイト） | 1,959 ns | 618 ns | 3.2 |
| 合法手V3 ID列（Pythonで1手ずつ `encode` → `ActionEncoderV3.legal_action_ids`） | 14.11 µs/局面 | 0.72 µs/局面 | 約20 |
| 準詰み（80局面、depth 3、30万node上限） | 1.56 s | 1.47〜1.51 s | 約1.05 |

準詰みの値・ノード数は比較元と完全一致しました。採用しなかった案（Boardの固定長化、共有treeのatomic削減など）と、その理由は [高速化レビュー](speed_review_20261004.md) の9章を参照してください。

## 2026-09-06: 合法手・特徴量・厳密めくれ探索

比較元は当時のmain `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc`、F1計測コードは `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` です。記録commit `49878b6` は計測コードのcommitとは異なります。

| 条件 | 値 |
|---|---|
| CPU / OS | Ryzen 9 7900X / Linux x86_64 |
| ツール | GCC 15.2.0、Python 3.12.1、CMake 4.2.3、pybind11 3.0.1 |
| ビルド | portable Release、`-O3 -DNDEBUG -std=c++17`、PERF/VERIFY OFF |
| LTO | native追加LTOはOFF。Python拡張は比較両側で既存pybind11 LTOを維持 |
| 実行 | CPU4に1thread固定、warmup 2回 |
| 集計 | 22 pairs / 11個の2-pair fixed-slot crossover blocks、block bootstrap 10,000回 |

時間は1実行の中央値、倍率は対応するblock比の中央値です。したがって時間の中央値同士の比とは一致しない場合があります。

| 処理（独立再測定） | 比較元 | F1候補 | 速度比［95%信頼区間］ |
|---|---:|---:|---:|
| 厳密めくれ・depth 7・100万node上限 | 2,505.49 ms | 1,051.16 ms | 2.373［2.361–2.403］ |
| Python StateFeaturizer・5万回 | 372.14 ms | 29.04 ms | 12.808［12.514–13.048］ |
| Python特徴量＋環境step・5万手 | 529.33 ms | 89.37 ms | 6.044［5.732–6.164］ |

厳密めくれのfixtureは `hidden_reserve`。cold探索をnode上限まで実行し `UNKNOWN` となる仕事で、詰みの完全証明時間ではありません。Pythonのfixtureは `reachable_32_seed42`。NN・GPU・問題保存・AI全体の時間は含みません。

同系列の探索終了時のLinux current RSSは70,152→49,208 KiBでした。peak RSSや累計確保量ではなく、別局面にも同じ削減率が成立するとは限りません。

次のnative合法手生成は同じF1比較の**正式系列**で、上の独立再測定とは別系列です。`midgame_250` を20万回処理した値です。

| C++内部処理 | 比較元 | F1候補 | 速度比［95%信頼区間］ |
|---|---:|---:|---:|
| 合法手count | 1,185,093 回/秒 | 3,112,938 回/秒 | 2.629［2.615–2.651］ |
| 合法手codes | 155,366 回/秒 | 378,400 回/秒 | 2.438［2.429–2.447］ |
| 合法手actions | 172,136 回/秒 | 346,226 回/秒 | 2.008［1.959–2.057］ |

1回は局面の合法手一覧または件数の生成です。Python `Game.legal_actions` のobject生成コストを含みません。

### 最終統合版との関係

最終統合commitは `8c173054dbd0f88977fb4cc12204e1cd2e88ac05`（PR #26）です。F1後のCI互換性修正でバイナリが変わっており、上記の数値を最終HEADの直接測定値とは扱いません。

CI修正版 `8b6dd8b` と保存済みF1候補を同じportable条件・CPU4・22 pairs / 11 blocksで比較した限定検証は次のとおりです。

| 固定順solver | CI修正版 / F1候補の速度比［95%信頼区間］ |
|---|---:|
| exact_reveal / hidden_reserve / depth 7 / 100万node上限 | 1.0103［0.9981–1.0269］ |
| visible_solver / five_moves / 10万node上限 | 1.0192［0.9973–1.0444］ |

全pairのsemantic digest・正しさcounterは一致しました。信頼区間は1を含み、この限定比較では明確な退行を検出していません。F1倍率と掛け合わせたり、全APIの非劣性や追加高速化へ一般化したりはしません。

原記録は統合commitに固定して参照できます。

- [F1報告・再現手順](https://github.com/kuboyoo/csplendor/blob/8c173054dbd0f88977fb4cc12204e1cd2e88ac05/doc/performance_experiments/final_main_vs_candidate_20260906.md)
- [F1 CSV](https://github.com/kuboyoo/csplendor/blob/8c173054dbd0f88977fb4cc12204e1cd2e88ac05/doc/performance_experiments/final_main_vs_candidate_20260906.csv)
- [F1 manifest](https://github.com/kuboyoo/csplendor/blob/8c173054dbd0f88977fb4cc12204e1cd2e88ac05/doc/performance_experiments/final_main_vs_candidate_manifest_20260906.json)
- [CI仕上げ報告](https://github.com/kuboyoo/csplendor/blob/8c173054dbd0f88977fb4cc12204e1cd2e88ac05/doc/performance_experiments/f4_ci_finish_review_20260906.md)

## 2026-08-30: Python API・自己対戦・詰み探索

Ryzen 9 7900X、GCC 15.2、Release、Python 3.12.1、CPU 1論理コア固定。合法手生成はseed 42・12手・合法手250件の中盤局面。best-of-5を7回、3 batch行った21標本の中央値です。

| 処理 | 測定値 |
|---|---:|
| Python `legal_actions` | 27,084 回/秒 |
| `legal_action_codes` 取得 | 125,444 回/秒 |
| `legal_action_count` 取得 | 1,011,935 回/秒 |
| C++内部適用の自己対戦 | 892,607 moves/sec |

自己対戦はseed 0–9の10ゲームを1標本とした90標本で、合法手生成とは別の仕事です。

別の厳密めくれ探索測定では、5手詰め収集局面の初手固定・深さ7・1実行1,000万node、warmup 2回後の15標本中央値で **5,440,074 nodes/sec、1.838秒**でした。9月の `hidden_reserve` とは局面と測定仕事が異なるため、数値を並べて退行・改善を判断できません。

出典: [当時のREADME](https://github.com/kuboyoo/csplendor/blob/7835f64/README.md)、[現在の性能テスト](../tests/test_perf.py)。

## 2026-08-04: MCTSのnative処理性能

Ryzen 9 7900X、GCC 13、portable Release。同じhost・seed・tree size・batch size、待ち時間ゼロのnative evaluator、5標本の中央値です。

| mode / backend | 高速化後の測定値 |
|---|---:|
| exact / legacy / 1 thread | 387,132 sim/s |
| exact / sharded / 1 thread | 222,253 sim/s |
| exact / sharded / 4 threads | 217,910 sim/s |
| exact / sharded / 8 threads | 194,405 sim/s |
| exact / root-parallel / 8 workers | 1,418,195 sim/s |
| determinized / legacy / 1 thread | 358,261 sim/s |
| determinized / sharded / 4 threads | 294,279 sim/s |
| determinized / root-parallel / 8 workers | 1,584,560 sim/s |

実モデル推論やGPU転送を含みません。root-parallelと共有treeは探索の構成が異なり、sim/sだけで同時間の棋力を比較できません。出典・比較元・実行条件は [MCTSホットパス高速化](mcts_hotpath_optimizations.md)。

## 手元で確認する

```bash
python -m pip install -e '.[dev]'
python -m pytest -m performance tests/test_perf.py --junitxml=build/performance.xml
```

XMLには `legal_actions_per_sec` などの測定値が記録されます。このテストは退行検出用で、F1のpaired A/Bとは異なる手順です。厳密な比較には上のF1報告のfixture・build条件・再現手順を使ってください。

測定値には対象commit、CPU、compiler、CPU target、Python版、局面・seed、支払いモード、thread数、warmup・標本数を添えます。異なる局面・仕事・計測経路の値を一律の「エンジン速度」に換算しません。
