# MiniVectorDB 本地与服务器验证记录

本文件汇总本次提交对应的验证过程与结果。**所有数字都是自测结果，不是官方平台成绩**；
官方成绩以平台对锁定 commit 的评测为准。两套环境的绝对耗时差异很大，原因写在
第 4 节，不要把本地的数字当成平台上的成绩。

## 1. 验证环境

| 环境 | 说明 |
| --- | --- |
| 本地 | Windows 11 + Docker Desktop（Linux 容器）。宿主机到 Linux 虚拟机的管道转发开销明显，见第 4 节 |
| 服务器 | Ubuntu 24.04，2 vCPU，约 1.97 GiB 内存，50 GiB 磁盘，Docker 29.1.3（原生 Linux，无虚拟机转发） |

两套环境使用同一份源码、同一个 Dockerfile 构建出的镜像，以及题包自带、未作任何修改的
`protocol_check/run.py`。

## 2. 公开用例（功能准入的本地预检）

命令：

```bash
python3 protocol_check/run.py --image minivectordb:latest --output protocol-results/public
```

结果：20 个公开用例全部 PASS，服务器上复跑结果一致。

```
sql_crud: PASS                 storage_restart: PASS
sql_comparisons: PASS          vector_euclidean: PASS
sql_ordering: PASS             transaction_commit: PASS
sql_empty: PASS                transaction_rollback: PASS
sql_primary_key: PASS          storage_mutations: PASS
sql_unicode: PASS              storage_multiple_tables: PASS
sql_bigint: PASS               vector_ties_and_filter: PASS
sql_unsupported: PASS          vector_float32: PASS
vector_index_maintenance: PASS transaction_failed_insert: PASS
transaction_autocommit_atomic: PASS
transaction_shutdown_rollback: PASS
```

握手检查 `python3 protocol_check/run.py --image minivectordb:latest --handshake-only` 同样通过。

## 3. 补充自测

公开用例每张表只有几行、只占一个页，覆盖不到分页、缓冲池换页与大数据量路径，
因此补了三个自测脚本。

### 3.1 崩溃恢复 `minivectordb/tests/crash_recovery.py`

用 `docker kill --signal=KILL` 模拟崩溃，复用同一个 `/data` 卷重启。五个阶段：

| 阶段 | 检查 | 结果 |
| --- | --- | --- |
| 1 | 确认提交后开启事务并写入，SIGKILL | — |
| 2 | 重启后已提交的行可见、未提交的行不可见、会话内可见集合稳定 | 全部通过 |
| 3 | 恢复途中再次 SIGKILL，再次重启（恢复幂等） | 全部通过 |
| 4 | 提交后不做检查点立刻 SIGKILL，重启后数据仍在 | 全部通过 |
| 5 | 事务内 UPDATE 后 ROLLBACK，再干净关闭并重启 | 全部通过 |

服务器上的实际输出：

```
phase 2: restart after the crash
  ok: the table survived the crash
  ok: only the committed rows are visible: [[1, 'kept'], [2, 'kept']]
  ok: the uncommitted update did not become visible: [1, 'kept']
  ok: the visible set is stable inside the session
phase 3: crash while the recovery is running
  ok: the recovery is idempotent: [[1, 'kept'], [2, 'kept']]
phase 4: crash directly after a commit and before the checkpoint
  ok: the acknowledged commit survived the crash: [[1, 'kept'], [2, 'kept'], [4, 'committed']]
phase 5: rollback and a clean shutdown
  ok: the rollback restored the row: [1, 'kept']
  ok: the clean shutdown kept the data: [[1, 'kept'], [2, 'kept'], [4, 'committed']]
crash recovery: all checks passed
```

### 3.2 可见行数

公开用例测不到「插入很多行」的场景，补了一个计数脚本：插入 N 行，统计会话内与
重启后能查到的行数。

| 数据量 | 报告插入 | 会话内可见 | 重启后可见 |
| --- | --- | --- | --- |
| 5,000 行 | 5,000 | 5,000 | 5,000 |
| 10,000 行 | 10,000 | 10,000 | 10,000 |

（这一项最初是 5,000 行只查得到 536 行，暴露了页号分配缺陷，修复过程见
`agent/conversation.json` 的实验 e10。）

### 3.3 召回率

`ann` 模式的返回集合与 `exact` 模式逐条比对，过滤组与不过滤组分别统计：

| 分组 | 平均 Recall@10 |
| --- | --- |
| 全部查询 | 1.0 |
| 带等值过滤 | 1.0 |
| 不过滤 | 1.0 |

题面要求两组平均 Recall@10 均不低于 0.95。

## 4. 为什么两套环境的耗时差别很大

最初在本地 Windows + Docker Desktop 上测到「每条查询 1.3~1.8 ms」，而且**与表规模
几乎无关**（100 行和 10,000 行一样慢）。这与「计算量随数据量增长」的预期矛盾，说明
���不是数据库的计算开销。把同一套脚本放到原生 Linux 服务器上，同样的查询只有
0.2~0.4 ms。结论是本地那 1.3 ms 来自 Windows 宿主机与 Linux 虚拟机之间的 docker CLI
管道转发，每条消息一次往返。

因此：

- 功能结论可以在两套环境之间迁移；
- 绝对耗时不可以迁移，文档里的性能数字以服务器上的数值为准；
- 任何本地测得的数字都不代表平台成绩。

## 5. 性能记录

见 [minivectordb-benchmark.md](minivectordb-benchmark.md)。
