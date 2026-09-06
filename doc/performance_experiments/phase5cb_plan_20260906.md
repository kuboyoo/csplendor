# 5C-B 実施前契約

基準: 1980b541bfde2db4d7f8fdba368b12013354881c (3E/5E採用、5A棄却)。
元dirty worktreeと直前のperf/action-selectionを保全し、perf/feature-tableで作業する。
REQUEST §17.2/17.3/19に従い、二案を独立評価する。公開schema/float加算順は変更しない。

1. immutable feature table: 90×8 floatとempty/hidden-level行。primaryは既存
   parallel_scheduler/five_moves/1 thread/20000 simulations (end-to-end)。
   state_encoder/midgame_250/100000 callsはmechanism micro、hidden/initialも照合。
   primary中央値>=1.03、95% block CI下端>1、独立再測定の再現を採用条件とする。
   microのみ改善なら棄却。solver、random selfplay、maskを2% regression guardとする。
2. Python境界: StateFeaturizer.featurizeのlist→NumPy変換をowning ndarrayで置換する。
   この既存consumerを実際に接続する。primaryは同API 50000回(32局面を循環、observer切替)、
   guardはfeaturize→合法手選択→applyの10000-step pipeline、native MCTS/solver。
   同じ3%+CI+独立再測定を要求。referenceは従来list→np.asarray。
   既存list APIは不変。未使用のcode-buffer/fill APIは追加しない。

両案とも旧encoder全196値のbitwise oracle、全90 card、empty、observer -1/0/1、
canonical swap、hidden-tier、invalid editor例外を照合する。public-card-statisticsの
pool順/float順は触らない。NumPyはfloat32/shape(196,)/C-contiguous/owningで、
次回呼出し・局面変更・search後も保持配列が不変であることを検査する。
out-bufferを受け取らないため、容量不一致や部分書込みの契約自体を新設しない。

既存paired runnerを再利用: smoke4 pairs、formal/holdout22 pairs、11 crossover blocks、
bootstrap10000、CPU4、warmup2。Pythonも同じJSON schema/runnerを用い、別統計基盤は作らない。
Release/reference/PERFを分離。採用版にnative/Python/ASAN+UBSANを実行。
perf不可ならN/A、NNや外部repoへの変更は対象外。棄却試作もdoc下に圧縮保存する。
最終累積比較は開始基準との直接比較、歴史的倍率の掛算なし。日本語commit、作業branch pushで終了。
