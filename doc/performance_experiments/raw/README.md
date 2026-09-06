# 性能実験raw記録

このディレクトリは、エンジン高速化ロードマップで生成したraw benchmark、診断ログ、
差分検証、stdout/stderr、計測時間およびdisassemblyを長期保存する。

## 保存規約

- 一意な計測記録を`/tmp`だけに置かない。build directoryと再生成可能なbinaryだけを
  `/tmp`へ置いてよい。
- 新しい記録は最初から`doc/performance_experiments/raw/<phase>/`へ出力する。
- 大きな確定済み記録は`gzip -n -9`で決定論的に圧縮する。raw内容自体は編集しない。
- 報告書は`/tmp`上の一時名ではなく、このディレクトリの保存先を参照する。
- `manifest.tsv`へ元ファイルのSHA-256、圧縮ファイルのSHA-256、双方のbyte数を記録する。

2026-09-02の初回移行では、Phase 0〜3Aと関連する探索・profiling・CTest記録
655件（元データ125.72 MiB、圧縮後15.79 MiB）を`/tmp`から移した。全ファイルで
gzip展開後のSHA-256が元記録と一致することを確認してから、`/tmp`側を削除した。

Phase 3Cでは、solver TT圧縮の正式A/B、layout、再profile、gprof、sanitizer、
5手・7手詰みを含む検証記録23件を`phase3c/`へ追加した。

## 構成

| directory | contents |
|---|---|
| `phase0/` | 基準測定、fixture、診断、再現性監査 |
| `phase1a/` | exact hash差分維持 |
| `phase1b/` | observable hash仮説 |
| `phase2a/` | rule transition小規模高速化 |
| `phase2b/` | 合法手生成高速化 |
| `phase3a/` | solver container/metadata削減 |
| `phase3b/` | 厳密めくれ探索の増分set-state/key、fallback、oracle検証 |
| `phase3c/` | solver TT key/entry圧縮、layout、Stage 2再profile |
| `phase3dp1/` | めくれscore一回計算、正式A/B・holdout・oracle・全回帰・sanitizer |
| `phase3dp2/` | 再帰scratch再利用、正式A/B・holdout・保持容量・全回帰 |
| `phase3d1/` | 通常着手rollback。直下v1・candidate_v2は棄却記録、candidate_v3がvisible-only採用版 |
| `exploratory/` | Phase分類前のsolver・benchmark探索 |

内容の確認例:

```bash
gzip -dc doc/performance_experiments/raw/phase3a/phase3a_solver_containers_final_20260902.json.gz \
  | jq '.complete, .cases | length'
```

整合性は`manifest.tsv`の`original_sha256`と、展開した内容のSHA-256を比較して確認する。
