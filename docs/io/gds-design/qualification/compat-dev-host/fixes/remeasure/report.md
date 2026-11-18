# cuFile qualification report

N/A is not a pass. Compat/simulator results are NOT GDS evidence.
Raw nvidia-fs counters require isolated operator attribution (AM-11).

## Failed commands (including partial CSVs)

- fix-B-3-t1-op4.csv: rc=124


## Required coverage (aggregate; N/A is incomplete, never PASS)

- G-1 lvm/device=0: **N/A** — missing cells=[{'gate': 'G-1', 'mount': 'lvm', 'device': '0', 'step': 'B-0'}]; cell statuses=[]
- G-2 lvm/device=0: **FAIL** — missing cells=[]; cell statuses=['N/A', 'N/A', 'N/A', 'N/A', 'FAIL', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A', 'N/A']
- G-3 lvm/device=0: **N/A** — missing cells=[]; cell statuses=['INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO', 'INFO']
- G-4 lvm/device=0: **N/A** — missing cells=[{'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'lvm', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'spill'}]; cell statuses=[]
- G-5 lvm/device=0: **N/A** — missing cells=[{'gate': 'G-5', 'mount': 'lvm', 'device': '0', 'step': 'B-1'}, {'gate': 'G-5', 'mount': 'lvm', 'device': '0', 'step': 'B-3'}]; cell statuses=[]
- G-7 lvm/device=0: **N/A** — missing cells=[{'gate': 'G-7', 'mount': 'lvm', 'device': '0', 'step': 'G-7', 'workload': 'sustained'}, {'gate': 'G-7', 'mount': 'lvm', 'device': '0', 'step': 'G-7', 'workload': 'restore'}, {'gate': 'G-7', 'mount': 'lvm', 'device': '0', 'step': 'G-7', 'workload': 'spill'}]; cell statuses=[]
- G-1 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-1', 'mount': 'unspecified', 'device': '0', 'step': 'B-0'}]; cell statuses=[]
- G-2 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-2', 'mount': 'unspecified', 'device': '0', 'step': 'B-3', 'engine': 'async'}, {'gate': 'G-2', 'mount': 'unspecified', 'device': '0', 'step': 'B-4', 'engine': 'async'}, {'gate': 'G-2', 'mount': 'unspecified', 'device': '0', 'step': 'B-5', 'engine': 'async'}, {'gate': 'G-2', 'mount': 'unspecified', 'device': '0', 'step': 'B-6', 'engine': 'async'}]; cell statuses=[]
- G-3 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-3', 'mount': 'unspecified', 'device': '0', 'step': 'B-3', 'engine': 'async'}, {'gate': 'G-3', 'mount': 'unspecified', 'device': '0', 'step': 'B-4', 'engine': 'async'}, {'gate': 'G-3', 'mount': 'unspecified', 'device': '0', 'step': 'B-5', 'engine': 'async'}, {'gate': 'G-3', 'mount': 'unspecified', 'device': '0', 'step': 'B-6', 'engine': 'async'}]; cell statuses=[]
- G-4 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'legacy', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '1', 'inode_mode': 'distinct', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'shared', 'workload': 'spill'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'sustained'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'single'}, {'gate': 'G-4', 'mount': 'unspecified', 'device': '0', 'step': 'B-10', 'arm': 'async_auto', 'writer_threads': '8', 'inode_mode': 'distinct', 'workload': 'spill'}]; cell statuses=[]
- G-5 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-5', 'mount': 'unspecified', 'device': '0', 'step': 'B-1'}, {'gate': 'G-5', 'mount': 'unspecified', 'device': '0', 'step': 'B-3'}]; cell statuses=[]
- G-7 unspecified/device=0: **N/A** — missing cells=[{'gate': 'G-7', 'mount': 'unspecified', 'device': '0', 'step': 'G-7', 'workload': 'sustained'}]; cell statuses=['N/A', 'N/A']

## Gates (individual cells)

| Gate | Result | Reason | Inputs |
|---|---|---|---|
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=1.0 | fix-B-3-t1-op1.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=1.0 | fix-B-3-t4-op1.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=2.0 | fix-B-3-t1-op2.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=2.0 | fix-B-3-t4-op2.csv:2 |
| G-2 | FAIL | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-3-t1-op4.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-3-t1-op4.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-3-t4-op4.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=25.0, MiB=8.0 | fix-B-3-t1-op8.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=25.0, MiB=8.0 | fix-B-3-t4-op8.csv:2 |
| G-2 | N/A | lvm/restore/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-4-t1-op4.csv:2 |
| G-2 | N/A | lvm/restore/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-4-t4-op4.csv:2 |
| G-2 | N/A | lvm/single/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-5-t1-op4.csv:2 |
| G-2 | N/A | lvm/single/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-5-t4-op4.csv:2 |
| G-2 | N/A | lvm/spill/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-6-t1-op4.csv:2 |
| G-2 | N/A | lvm/spill/device=0 engine_fraction=1.0 (routed legacy/sync if <0.99 or legacy_bytes>0); ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=32.0, MiB=4.0 | fix-B-6-t4-op4.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.98575; requires >=0.9; refill_gap/backlog=0.17101504395974088 (<=0.1); backlog_ns=4814511887.0; engine_fraction=1.0 | fix-B-3-t1-op1.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=6.457 (ceiling 70); residence_p99_us=2097.152; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t1-op1.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.7529375; requires >=0.9; refill_gap/backlog=0.9894782760817371 (<=0.1); backlog_ns=5379279331.0; engine_fraction=1.0 | fix-B-3-t4-op1.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=14.475 (ceiling 70); residence_p99_us=4194.304; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t4-op1.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.80390625; requires >=0.9; refill_gap/backlog=0.9016642428769971 (<=0.1); backlog_ns=4821968182.0; engine_fraction=1.0 | fix-B-3-t1-op2.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=17.849 (ceiling 70); residence_p99_us=8388.608; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t1-op2.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.79878125; requires >=0.9; refill_gap/backlog=0.9441484008677032 (<=0.1); backlog_ns=5393048197.0; engine_fraction=1.0 | fix-B-3-t4-op2.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=18.033 (ceiling 70); residence_p99_us=8388.608; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t4-op2.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.782125; requires >=0.9; refill_gap/backlog=0.9230463319049745 (<=0.1); backlog_ns=4609987591.0; engine_fraction=1.0 | fix-B-3-t1-op4.csv:2 |
| G-5a | FAIL | lvm/sustained/device=0 queries_per_op=26.954 (ceiling 70); residence_p99_us=16777.216; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t1-op4.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.782125; requires >=0.9; refill_gap/backlog=0.9230463319049745 (<=0.1); backlog_ns=4609987591.0; engine_fraction=1.0 | fix-B-3-t1-op4.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=26.954 (ceiling 70); residence_p99_us=16777.216; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t1-op4.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.76965625; requires >=0.9; refill_gap/backlog=0.9801127726496617 (<=0.1); backlog_ns=5426068305.0; engine_fraction=1.0 | fix-B-3-t4-op4.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=27.831 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t4-op4.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.82892; requires >=0.9; refill_gap/backlog=0.8527108916615822 (<=0.1); backlog_ns=4575743703.0; engine_fraction=1.0 | fix-B-3-t1-op8.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=28.191 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t1-op8.csv:2 |
| G-3 | INFO | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.90448; requires >=0.9; refill_gap/backlog=0.6622997207758943 (<=0.1); backlog_ns=5410763859.0; engine_fraction=1.0 | fix-B-3-t4-op8.csv:2 |
| G-5a | PASS | lvm/sustained/device=0 queries_per_op=21.508 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-3-t4-op8.csv:2 |
| G-3 | INFO | lvm/restore/device=0 time-integral backlog mean/Q_eff=0.79559375; requires >=0.9; refill_gap/backlog=0.8639860212062327 (<=0.1); backlog_ns=4800185847.0; engine_fraction=1.0 | fix-B-4-t1-op4.csv:2 |
| G-5a | PASS | lvm/restore/device=0 queries_per_op=32.531 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-4-t1-op4.csv:2 |
| G-3 | INFO | lvm/restore/device=0 time-integral backlog mean/Q_eff=0.78865625; requires >=0.9; refill_gap/backlog=0.9522360813292139 (<=0.1); backlog_ns=5154470233.0; engine_fraction=1.0 | fix-B-4-t4-op4.csv:2 |
| G-5a | PASS | lvm/restore/device=0 queries_per_op=33.796 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-4-t4-op4.csv:2 |
| G-3 | INFO | lvm/single/device=0 time-integral backlog mean/Q_eff=0.78921875; requires >=0.9; refill_gap/backlog=0.8183219066070375 (<=0.1); backlog_ns=4321553586.0; engine_fraction=1.0 | fix-B-5-t1-op4.csv:2 |
| G-5a | PASS | lvm/single/device=0 queries_per_op=42.805 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-5-t1-op4.csv:2 |
| G-3 | INFO | lvm/single/device=0 time-integral backlog mean/Q_eff=0.75759375; requires >=0.9; refill_gap/backlog=0.9693409068311211 (<=0.1); backlog_ns=5271441693.0; engine_fraction=1.0 | fix-B-5-t4-op4.csv:2 |
| G-5a | PASS | lvm/single/device=0 queries_per_op=47.016 (ceiling 70); residence_p99_us=33554.432; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-5-t4-op4.csv:2 |
| G-3 | INFO | lvm/spill/device=0 time-integral backlog mean/Q_eff=0.78584375; requires >=0.9; refill_gap/backlog=0.8061393855481013 (<=0.1); backlog_ns=3141285793.0; engine_fraction=1.0 | fix-B-6-t1-op4.csv:2 |
| G-5a | PASS | lvm/spill/device=0 queries_per_op=20.666 (ceiling 70); residence_p99_us=0.000; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-6-t1-op4.csv:2 |
| G-3 | INFO | lvm/spill/device=0 time-integral backlog mean/Q_eff=0.83365625; requires >=0.9; refill_gap/backlog=0.8854683734080984 (<=0.1); backlog_ns=4989787581.0; engine_fraction=1.0 | fix-B-6-t4-op4.csv:2 |
| G-5a | PASS | lvm/spill/device=0 queries_per_op=15.456 (ceiling 70); residence_p99_us=0.000; AM-15 subcriterion only; not NFR-3/NFR-4 qualification | fix-B-6-t4-op4.csv:2 |
| G-7 | N/A | ('unspecified', '0', '', 'soak-restore.csv.soak.csv'): duration=31.1997s, warmup=60s, samples=0, tolerance=0.01; instability=[], missing=['arena_bytes', 'registered_bytes', 'unregistered_bytes', 'pinned_host_bytes', 'op_pool_live', 'lane_count', 'thread_count', 'fd_count', 'device_used_bytes', 'epoch summaries'], errors=False, no_engine_progress=False | soak-restore.csv.soak.csv:10, soak-restore.csv.soak.csv:11, soak-restore.csv.soak.csv:12, soak-restore.csv.soak.csv:13, soak-restore.csv.soak.csv:14, soak-restore.csv.soak.csv:2, soak-restore.csv.soak.csv:3, soak-restore.csv.soak.csv:4, soak-restore.csv.soak.csv:5, soak-restore.csv.soak.csv:6, soak-restore.csv.soak.csv:7, soak-restore.csv.soak.csv:8, soak-restore.csv.soak.csv:9 |
| G-7 | N/A | ('unspecified', '0', '', 'soak-spill.csv.soak.csv'): duration=32.5916s, warmup=60s, samples=0, tolerance=0.01; instability=[], missing=['arena_bytes', 'registered_bytes', 'unregistered_bytes', 'pinned_host_bytes', 'op_pool_live', 'lane_count', 'thread_count', 'fd_count', 'device_used_bytes', 'epoch summaries'], errors=False, no_engine_progress=False | soak-spill.csv.soak.csv:10, soak-spill.csv.soak.csv:11, soak-spill.csv.soak.csv:12, soak-spill.csv.soak.csv:2, soak-spill.csv.soak.csv:3, soak-spill.csv.soak.csv:4, soak-spill.csv.soak.csv:5, soak-spill.csv.soak.csv:6, soak-spill.csv.soak.csv:7, soak-spill.csv.soak.csv:8, soak-spill.csv.soak.csv:9 |
| G-1 | N/A | No applicable input rows |  |
| G-4 | N/A | No applicable input rows |  |
| G-5 | N/A | No applicable input rows |  |
| G-6 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |
| G-8 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |

## Matched baseline throughput

| Mount | Workload | Variant | GiB/s | Baseline GiB/s | Ratio | Source |
|---|---|---|---|---|---|---|
| lvm | sustained | registered | 5.791 | None | None | fix-B-3-t1-op1.csv |
| lvm | sustained | registered | 6.656 | None | None | fix-B-3-t4-op1.csv |
| lvm | sustained | registered | 6.46 | None | None | fix-B-3-t1-op2.csv |
| lvm | sustained | registered | 6.616 | None | None | fix-B-3-t4-op2.csv |
| lvm | sustained | registered | 6.367 | None | None | fix-B-3-t1-op4.csv |
| lvm | sustained | registered | 6.367 | None | None | fix-B-3-t1-op4.csv |
| lvm | sustained | registered | 6.547 | None | None | fix-B-3-t4-op4.csv |
| lvm | sustained | registered | 6.379 | None | None | fix-B-3-t1-op8.csv |
| lvm | sustained | registered | 6.541 | None | None | fix-B-3-t4-op8.csv |
| lvm | restore | registered | 6.301 | None | None | fix-B-4-t1-op4.csv |
| lvm | restore | registered | 6.57 | None | None | fix-B-4-t4-op4.csv |
| lvm | single | registered | 6.256 | None | None | fix-B-5-t1-op4.csv |
| lvm | single | registered | 6.614 | None | None | fix-B-5-t4-op4.csv |
| lvm | spill | registered | 3.166 | None | None | fix-B-6-t1-op4.csv |
| lvm | spill | registered | 4.964 | None | None | fix-B-6-t4-op4.csv |

## AM-5 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
| lvm/0 | sustained/async/registered | 1.0 (1.0) | slot | 32 | 5.791 | not reported | fix-B-3-t1-op1.csv |
| lvm/0 | sustained/async/registered | 1.0 (1.0) | slot | 32 | 6.656 | not reported | fix-B-3-t4-op1.csv |
| lvm/0 | sustained/async/registered | 2.0 (2.0) | slot | 32 | 6.460 | not reported | fix-B-3-t1-op2.csv |
| lvm/0 | sustained/async/registered | 2.0 (2.0) | slot | 32 | 6.616 | not reported | fix-B-3-t4-op2.csv |
| lvm/0 | sustained/async/registered | 4.0 (4.0) | slot | 32 | 6.367 | not reported | fix-B-3-t1-op4.csv |
| lvm/0 | sustained/async/registered | 4.0 (4.0) | slot | 32 | 6.367 | not reported | fix-B-3-t1-op4.csv |
| lvm/0 | sustained/async/registered | 4.0 (4.0) | slot | 32 | 6.547 | not reported | fix-B-3-t4-op4.csv |
| lvm/0 | sustained/async/registered | 8.0 (8.0) | slot | 32 | 6.379 | not reported | fix-B-3-t1-op8.csv |
| lvm/0 | sustained/async/registered | 8.0 (8.0) | slot | 32 | 6.541 | not reported | fix-B-3-t4-op8.csv |
| lvm/0 | restore/async/registered | 4.0 (4.0) | slot | 32 | 6.301 | not reported | fix-B-4-t1-op4.csv |
| lvm/0 | restore/async/registered | 4.0 (4.0) | slot | 32 | 6.570 | not reported | fix-B-4-t4-op4.csv |
| lvm/0 | single/async/registered | 4.0 (4.0) | slot | 32 | 6.256 | not reported | fix-B-5-t1-op4.csv |
| lvm/0 | single/async/registered | 4.0 (4.0) | slot | 32 | 6.614 | not reported | fix-B-5-t4-op4.csv |
| lvm/0 | spill/async/registered | 4.0 (4.0) | slot | 32 | 3.166 | not reported | fix-B-6-t1-op4.csv |
| lvm/0 | spill/async/registered | 4.0 (4.0) | slot | 32 | 4.964 | not reported | fix-B-6-t4-op4.csv |

## AM-9 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|

## AM-10 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
