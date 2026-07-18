# 07. Codexモデル割当とトークン節約方針

> この文書の割当判断担当: **Codex Sol Ultra**

## 1. 利用可能モデルと訂正

この利用環境で実際に選択できる上位モデルは **Sol Ultra** である。以前の計画に記載した
下位tier名は選択肢として存在しないため、本計画の全phase、実装、test、reviewをSol Ultraへ
統一する。

トークン節約はモデルを下げることではなく、次で行う。

- phaseを跨がず、1回のtaskを1つのcontractへ限定する。
- 対象file、関連test、失敗trace、未解決decisionだけを渡す。
- 成功logは全文ではなくcommand、version、件数、digestを残す。
- 実装sessionとreview sessionを分け、reviewerにはdiffとinvariantを渡す。
- 定型作業も狭いcontextのSol Ultra sessionとして独立させる。

## 2. 全phaseの割当

| Part | 主作業 | 実装担当 | Review | 渡すcontextの重点 |
|---|---|---|---|---|
| PS-0 | baseline/digest/probe | Sol Ultra | 別Sol Ultra session | fixture、benchmark条件、互換contract |
| PS-1 | hash/world/availability | Sol Ultra | 別Sol Ultra session | TreeKey、feature、world-local mask |
| PS-2 | CMake/CTest/sanitizer | Sol Ultra | 別Sol Ultra session | target、flags、sanitizer log |
| PS-3 | seed/RNG | Sol Ultra | 別Sol Ultra session | domain/version/golden/replay契約 |
| PS-4 | lifecycle/snapshot/API | Sol Ultra | 別Sol Ultra session | active、GIL、config、generation |
| PS-5 | serial NodeRecord/reservation/ticket | Sol Ultra | 別Sol Ultra session | exactly-once、VL、Pendingとticket分離 |
| PS-6 | root-parallel oracle | Sol Ultra | 別Sol Ultra session | root N/Q merge、budget、memory |
| PS-7 | coarse shared tree | Sol Ultra | 別Sol Ultra session | 全shared state、linearization、TSAN |
| PS-8 | scheduler/coordinator | Sol Ultra | 別Sol Ultra session | shutdown、queue、exception、lock境界 |
| PS-9 | deterministic replay | Sol Ultra | 別Sol Ultra session | event order、reduction、serializer |
| PS-10 | shard/node lock | Sol Ultra | 別Sol Ultra session | lifetime、lock order、publication |
| PS-11 | Python binding | Sol Ultra | 別Sol Ultra session | GIL、callback、ownership、cleanup |
| PS-12 | benchmark/quality | Sol Ultra | 別Sol Ultra session | raw結果、VL tuning、go/no-go |
| PS-13 | final audit/rollout | 実装者と別のSol Ultra session | - | concurrency/hash/GIL横断監査 |

同じモデルでもsessionを分ける理由は、実装者の前提をreviewerが無批判に引き継がないためである。

## 3. 文書別割当

| 文書 | 更新タイミング | 担当 |
|---|---|---|
| `README.md`（本directory） | ADR/phase/状態変更 | Sol Ultra |
| `01_current_state_and_decisions.md` | blocker/ADR更新 | Sol Ultra |
| `02_parallel_architecture.md` | state/lock/API変更 | Sol Ultra |
| `03_rng_and_determinization.md` | key/RNG/world変更 | Sol Ultra |
| `04_implementation_phases.md` | gate/順序変更 | Sol Ultra |
| `05_validation_and_tsan.md` | test/toolchain更新 | Sol Ultra |
| `06_benchmark_and_rollout.md` | benchmark/採否 | Sol Ultra |
| `07_codex_model_assignment.md` | 委譲構造変更 | Sol Ultra |

## 4. Phase context bundle

全repoや長い会話履歴を毎回渡さない。各phaseの開始時には次を1つのbundleにする。

```text
Part ID / objective
計画書の該当節
変更を許可するfile
確定API/signature
invariantと禁止事項
関連する既存test
追加すべきtest
実行command
直前phaseのhandoff summary
未解決decision 1〜3件
```

### Architecture/contract task

```text
該当ADR
対象header
既存contract test
最小failure fixture/trace
期待するlinearization point
完了gate
```

### TSAN failure task

```text
完全な最初のTSAN report
最小reproducer
seed/nonce/scheduler trace
対象diff
lock/state machine仕様
compiler/runtime/version
再現command
```

TSAN logは最初のwrite/read stack、thread creation stack、held stateを省略しない。一方、成功した
大量test logはcommandとdigestだけにする。

### Final review task

```text
全phaseのADR一覧
final diff statと対象file
sanitizer summary/artifact
replay digest
benchmark/quality report
known limitations
rollout/rollback config
```

## 5. Review独立性

少なくとも次のphaseは実装担当とreview担当を別Sol Ultra sessionにする。

- PS-1 information-set correctness。
- PS-4 lifecycle/GIL。
- PS-5 reservation/ticket。
- PS-7 shared tree。
- PS-8 shutdown/coordinator。
- PS-10 sharding。
- PS-11 Python binding。
- PS-13 final audit。

reviewerへ実装者の結論だけを渡さず、contract、diff、test、failure traceを渡す。reviewerは独立に
次を追う。

```text
all shared mutable fields
all state transitions
all exception/cancel exits
all lock acquisition paths
all pointer/handle lifetimes
all RNG consumers
all GIL acquire/release points
all stale/duplicate result paths
```

## 6. 1 phaseの推奨進め方

### Step A: contract freeze

- 該当ADRとinvariantを確定する。
- API、linearization point、failure behaviorを決める。
- test caseとexit gateを先に書く。
- auditで計画と現行codeの差が判明したら計画を先に直す。

### Step B: implementation

- 指定file/contract内だけ変更する。
- 現行`csplendor_core`が`INTERFACE` targetの間はparallel coreをheader-onlyで実装する。
- normal testを先に通し、shared stateを導入したphaseはsanitizerも実行する。
- handoff summaryを作る。

### Step C: focused review

- 別Sol Ultra sessionがdiffとinvariantを照合する。
- race/lifetime/failure pathを重点確認する。
- testがimplementationを追認しただけになっていないか確認する。
- gate合格または差戻しを明示する。

### Step D: mechanical close

- 狭いcontextのSol Ultra sessionで文書/index/結果表を更新する。
- commandとartifact digestを整理する。
- 次phaseに不要なhistoryはhandoffへ含めない。

## 7. 停止・計画修正条件

次の場合は実装を惰性で進めず、同じphase内でcontract・計画・test oracleを更新する。

- 計画書と現行codeのcontractが食い違う。
- public API/ABI変更が必要。
- hidden state、observer、mask、leaf keyの意味が曖昧。
- lockを新しく二つ以上同時保持する必要がある。
- memory orderを選ぶ必要がある。
- RAII cleanupとcallback exceptionの順序が曖昧。
- TSAN reportまたはhangが出る。
- deterministic replay digestがずれる。
- golden testを更新しないと実装できない。
- performance改善のためcorrectness contractを緩めたくなる。
- project sourceをTSAN suppressionしたくなる。
- active search中に待機するAPIが必要になる。

この停止はblockではなく、予想外の問題を計画へ反映してから安全に再開するためのgateである。

## 8. Prompt template

### Contract/review用

```text
対象Part: PS-X
目的: ...
正本: doc/parallel_search_plan/... の該当節
対象file: ...
必須invariant: ...
禁止: public contract変更、suppression、silent retry等
入力: diff / failing trace / tests
出力: findingを重大度順、修正案、追加test、go/no-go
曖昧なcontractは実装せず明示すること
```

### 実装用

```text
対象Part: PS-X
変更可能file: ...
確定API/signature: ...
実装手順: ...
必須test: ...
実行command: ...
完了条件: ...
scope外の最適化・refactorを行わないこと
contract差が判明したら計画とoracleを先に更新すること
```

### 定型作業用

```text
入力artifact: ...
更新先とformat: ...
数値の計算規則: ...
意味論や閾値を変更しないこと
失敗/欠損は推測せず報告すること
```

## 9. Token節約の具体策

- 全historyでなく、該当計画節 + diff + failing traceを渡す。
- contract確定前に複数案を実装しない。
- coarse版をoracleにし、sharded版reviewで議論をやり直さない。
- 失敗scheduleをtrace化し、長い自然言語説明を減らす。
- 成功logは全文でなくcommand、version、digest、summaryを残す。
- benchmark raw data整形、test matrix実行、文書更新を別の狭いtaskへ分ける。
- reviewerはgenerated boilerplateでなくstate/lock/lifetime diffへ集中する。
- phaseを跨ぐ「ついでの最適化」を禁止し、context膨張を防ぐ。
- sub-agentを使う場合も、計画書を丸ごと渡さず担当節と許可fileだけを渡す。

## 10. 最終割当判断

全作業の推奨モデルは **Codex Sol Ultra** である。トークン量はモデルtierではなく、phase単位の
context分割、成果物digest、独立review、変更範囲制限で抑える。特にshared state、hidden
information、GIL、sanitizer原因解析は、狭いcontextにしても判断精度を下げない。
