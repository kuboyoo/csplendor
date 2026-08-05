# R6 solver / puzzle tooling 分割記録

## 目的と固定した契約

R6では、solverの探索アルゴリズムを変更せず、公開facadeと内部実装、DFPNの探索補助、
詰み問題生成の責務を個別に変更・検証できる境界へ分割した。次を固定契約とした。

- visible-only / reveal-verifiedの解、score、action順、limit理由
- reveal-verifiedのhidden outcome、oracle内部表現、proof DAG byte相当JSON
- `scripts.dfpn_mate_solver`の既存import、CLI option、stdout/stderr/exit code
- `scripts.generate_mate_puzzles`の既存import pathと生成ファイル形式
- public solverのcopy/move可否

visible-onlyとreveal-verifiedはhidden情報の意味が異なるため、探索本体を共通化していない。
共通化したのは値として同一のsearch limit、state-key core、action-order prefix、
zero-sum score/tie、terminal resultに限定した。

## C++の変更

- `solver_types.h`へ公開result型と共通solver value typeを集約した。
- `visible_only_solver.h`を26行、`reveal_verified_solver.h`を36行のPImpl facadeにし、
  非template実装をそれぞれ`.cpp`へ移した。
- 旧header-only classが持っていたcopy construct/assignmentとnoexcept moveを明示的に維持した。
- `reveal_solver_components`へ次を分離した。
  - 初期既知/hidden card、claimed/unseen/acquired-hidden集合
  - oracle action metadataと順序
  - proof DAG node/edge所有権と上限判定
- `csplendor_core`とPython extensionの双方が同じcompiled sourceを利用する。

proof DAGの再帰探索とvalidationはreveal-verified固有の意味を持つため、solver側に残した。
componentはstorage、limit、card分類という独立してテスト可能な責務だけを所有する。

## Python toolingの変更

DFPNは既存の`dfpn_mate_solver.py`を互換facadeとして維持し、次を分離した。

| component | 責務 |
|---|---|
| `dfpn_types.py` | node、stats、limit例外 |
| `dfpn_table.py` | 置換表の型とlifetime。hot pathはnative `dict`を維持 |
| `dfpn_proof.py` | OR/ANDのproof/refutation child選択とtree抽出 |
| `dfpn_output.py` | strategy DAG codec、KIFU出力（先行分割を維持） |
| `dfpn_cli.py` | argument parsingとCLI orchestration（先行分割を維持） |

詰み問題生成は、`generate_mate_puzzles.py`の既存関数をforwarding surfaceとして維持し、
候補playout、Genbu外部adapter、解の一意性/countermate検証、atomic永続化を
`puzzle_candidates.py`、`puzzle_engine_adapter.py`、`puzzle_validation.py`、
`puzzle_persistence.py`へ分離した。通常の候補生成・保存component importは
`dlsplendor`やmodel fileを探索せず、Genbu adapterを実際に構築した時だけ探索する。

## 同値性と境界値

`tests/test_solver_refactor_contracts.py`でelapsed timeだけを除いたJSONを固定した。

| fixture | 固定内容 | SHA-256 |
|---|---|---|
| visible one-move win | winner、depth、`[66,37,6]`、stats | `e59ea8ee05c26d7389eaba07c3ce812db4ef21e9bd98fb93c50b813610e0fed3` |
| reveal one-move win | proven、`[692,37]`、validated proof DAG | `9732dd60583389a9220ae450619dcaecadc1defc73f6c9cfcbec128173fe2ba2` |
| visible node limit=1 | unknown理由、空line、stats | `5a07ad46e3fda32a97a7471378fd82c5758471e3769de259815b9d5a4102ebdb` |

node limitとtime limitはそれぞれ`node limit exceeded`、`time limit exceeded`のまま
区別される。solverには公開cancel APIがなく、Python root-parallel DFPNのfuture cancelと
worker shutdown経路は変更していない。既存のsequential/parallel同値testを維持した。

CLI/outputは既存の`test_phase6_dfpn_modules.py`でstdout/stderr/exit code、strategy JSON、
KIFU digestを継続固定する。puzzle testはdepth directory、problem/strategy/KIFU、manifest、
hidden reserved cardの再現性を固定する。

## 性能

Ubuntu x86_64、Python 3.12、NumPy 2.5.1、同一venv依存、portable Release extensionで、
`origin/main` (`ebaa876`)と本変更を`benchmark_solvers.py`により5回測定した。
各値はnodes/s中央値である。

| workload | main | R6 | 比率 |
|---|---:|---:|---:|
| visible-only（1,000 calls/sample） | 905,171 | 906,794 | 1.002 |
| reveal-verified（100 calls/sample） | 308,793 | 303,605 | 0.983 |
| Python DFPN（1,000 nodes） | 776.7 | 784.4 | 1.010 |

peak RSSは354,720 KiBから355,204 KiB（1.001）である。revealの差は短時間fixtureの
測定揺らぎを含み、停止基準0.97とRSS 5%を満たす。解、node数、action順は同一である。

## 検証

- editable install
- Python solver / DFPN / puzzle / CLI targeted test
- native GCC 13 test 13件
- public header、copy/move、hidden/oracle/proof component unit test
- node/time limit、solution/action/proof golden test
- `benchmark_solvers.py`による旧新比較

全体pytest、strict GCC/Clang、sanitizer、package、全platform CIはPRのmerge gateとする。
