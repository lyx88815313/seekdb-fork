# MiniVectorDB

一个支持 SQL、持久化存储与事务的轻量向量数据库，实现了 `2026-competition-round2`
赛题要求的全部功能范围：基础 SQL、向量 TopK 检索、向量索引、显式事务、崩溃恢复与
受内存约束下的存储引擎。

程序通过 stdin/stdout 的 JSON Lines 协议与判题器通信，协议细节见
[`STDIO_PROTOCOL.md`](STDIO_PROTOCOL.md)。

## 语言与构建

| 项目 | 说明 |
| --- | --- |
| 语言 | C++17（GNU C++，`<bits/stdc++.h>` 之外的常用标准头文件） |
| 编译器 | `g++-13`（Ubuntu 24.04 仓库版本 `13.3.0-6ubuntu2~24.04.1`） |
| 编译选项 | `-O2 -std=c++17 -Wall -Wextra` |
| 第三方依赖 | 无。只使用 C++ 标准库与 POSIX 文件接口 |
| 目标平台 | Linux x86_64 |

编译脚本为 [`minivectordb/build.sh`](minivectordb/build.sh)：

```bash
sh minivectordb/build.sh                 # 生成 build/minivectordb
CXX=clang++ sh minivectordb/build.sh     # 也可以换用其它 C++17 编译器
```

## 镜像构建与启动

仓库根目录的 [`Dockerfile`](Dockerfile) 是一个两阶段构建：编译阶段从仓库内的
`minivectordb/` 源码生成二进制，运行阶段只携带该二进制。基础镜像按 digest 固定，
编译包版本固定，因此镜像内容可复现；运行期不联网、不需要任何第三方库。

```bash
# 构建镜像
docker build -t minivectordb:latest .

# 前台启动并保持标准输入打开（判题器通过进程管道通信）
docker run --rm -i \
  -v minivectordb-data:/data \
  --cpus=2 \
  --memory=2g \
  minivectordb:latest
```

入口点是 `/app/minivectordb`，默认参数 `--data-dir /data`。容器启动后程序先完成
恢复，再向 stdout 输出一行 `{"type":"ready"}` 并 flush，之后逐行读取 JSON 请求、
逐行返回 JSON 响应；日志与调试信息只写 stderr。

数据目录：

| 路径 | 内容 |
| --- | --- |
| `/data` | 数据目录。表数据文件、预写日志与清单文件都在这里 |
| `/data/t_<id>.dat` | 每张表的堆文件，由固定大小的页组成 |
| `/data/wal.log` | 预写日志（redo log） |
| `/data/manifest` | 检查点信息（已落盘的 LSN） |

环境变量 `VECTORDB_BUFFER_POOL_MB` 可以调整缓冲池大小（默认 512 MiB 上限，实际按需
分配）；`VECTORDB_DATA_DIR` 可以覆盖数据目录（本地用 `--command` 方式自测时使用）。

启动时会先重放预写日志、重建目录与向量索引，**重建完成后才输出 `{"type":"ready"}`**。
重建的规模与耗时写在 stderr 上，例如：

```text
minivectordb: rebuilt the vector index of table docs (10000 rows) in 1271 ms
```

## SQL 支持范围

支持（题目要求的子集）：

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(32), embedding VECTOR(4));
CREATE VECTOR INDEX t_vec ON t(embedding);
DROP TABLE t;

INSERT INTO t VALUES (1, 'a', '[1, 0, 0, 0]'), (2, 'b', '[0, 1, 0, 0]');
INSERT INTO t (id, name) VALUES (3, 'c');

SELECT id, name FROM t;
SELECT * FROM t WHERE id >= 2 AND name <> 'x';      -- 仅 = < <= > >=
SELECT id, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
  FROM t WHERE name = 'a' ORDER BY distance LIMIT 2;

UPDATE t SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM t WHERE id = 2;

BEGIN;
UPDATE t SET name = 'tx' WHERE id = 1;
COMMIT;      -- 或 ROLLBACK
```

规则细节：

- 一张表恰好一个 `BIGINT PRIMARY KEY`，最多一个 `VECTOR` 列；主键不可重复。
- `VARCHAR(n)` 的长度按 Unicode 码点数计算；字符串比较按 Unicode 码点顺序
  （UTF-8 字节序与码点序一致）。
- 向量是定长 `float32` 数组：输入必须可表示为有限 `float32`，拒绝 `NaN`、
  `Infinity`、维度不符以及转换溢出。距离按**存储后**的向量计算，`L2_DISTANCE`
  返回欧氏距离（不是距离平方）。
- `ORDER BY` 的并列项按主键升序排列；未指定 `ORDER BY` 时结果按主键升序输出。
- 过滤后的 TopK 在满足 `WHERE` 的记录集合内计算，不存在“先取 K 条再过滤”。
- 精确（`exact`）与近似（`ann`）检索由请求的 `search_mode` 区分，默认 `exact`；
  `ann` 模式下使用向量索引，索引缺失或候选不足时回退到精确扫描，保证结果正确。
- 不支持的语句（`ALTER TABLE`、`JOIN`、聚合、子查询、`DISTINCT`、嵌套事务等）
  返回 `ok:false` 与错误码，不会被静默解释成别的含义。

## 错误处理

失败时响应形如：

```json
{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key 1"}}
```

`error.code` 恒为非空字符串，常用取值：

| 错误码 | 含义 |
| --- | --- |
| `duplicate_key` | 主键重复 |
| `unsupported_sql` | 语句或子句不在支持范围内 |
| `dimension_mismatch` | 向量维度与列定义或查询向量不符 |
| `invalid_vector` | 向量字面量非法（非有限值、溢出、非数字） |
| `type_mismatch` | 字面量类型与列类型不匹配 |
| `value_too_long` | 字符串超过 `VARCHAR(n)` 长度 |
| `column_not_found` / `table_not_found` | 列或表不存在 |
| `syntax_error` | 语句无法解析 |
| `internal_error` / `io_error` | 内部或 I/O 故障 |

出错的语句不会留下任何部分修改：语句级回滚只撤销该语句的改动，显式事务中此前
成功的语句仍然可以提交或回滚。

## 从空数据目录开始的操作示例

```bash
mkdir -p /tmp/mvdb-data
docker run --rm -i -v /tmp/mvdb-data:/data minivectordb:latest <<'JSONL'
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, 'a', 'hello', '[3,4]'), (2, 'b', 'world', '[0,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"UPDATE docs SET content = 'tx' WHERE id = 1;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":7,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":8,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":9,"op":"shutdown"}
JSONL
```

输出（每行一个 JSON 报文）：

```text
{"type":"ready"}
{"id":1,"ok":true,"affected_rows":0}
{"id":2,"ok":true,"affected_rows":2}
{"id":3,"ok":true,"columns":["id","content"],"rows":[[1,"hello"],[2,"world"]]}
{"id":4,"ok":true,"affected_rows":0}
{"id":5,"ok":true,"affected_rows":1}
{"id":6,"ok":true,"affected_rows":0}
{"id":7,"ok":true,"columns":["id","content"],"rows":[[1,"hello"],[2,"world"]]}
{"id":8,"ok":true,"columns":["id","distance"],"rows":[[2,0],[1,5]]}
{"id":9,"ok":true}
```

数值一律按最短形式输出：整数值输出为 JSON 整数，非整数值输出为浮点数。
距离是欧氏距离，所以 `[3,4]` 到 `[0,0]` 的距离是 5 而不是 25。

重启后数据保持不变：再次启动同一个 `/data` 目录即可。

## 自测

题包自带的公开用例与协议检查工具在仓库根目录：

```bash
docker build -t minivectordb:latest .
python3 protocol_check/run.py --image minivectordb:latest --handshake-only --output "protocol-results/handshake"
python3 protocol_check/run.py --image minivectordb:latest --output "protocol-results/public"
```

> 题包自带的驱动使用 `selectors` 监听子进程管道，在 Windows 上无法运行
> （`WinError 10093`），需要在 Linux 里执行。

本仓库另外提供两个自测脚本：

```bash
# 崩溃恢复：SIGKILL、未提交事务、恢复途中再崩溃、提交后立刻崩溃、干净关闭
python3 minivectordb/tests/crash_recovery.py --image minivectordb:latest

# 本地性能记录（非官方平台成绩）
python3 minivectordb/benchmark/benchmark.py --image minivectordb:latest
```

设计说明、验证记录与已知限制见 [`docs/minivectordb-design.md`](docs/minivectordb-design.md)，
验证过程见 [`docs/minivectordb-verification.md`](docs/minivectordb-verification.md)，
本地性能记录见 [`docs/minivectordb-benchmark.md`](docs/minivectordb-benchmark.md)。

Agent prompt 材料在 [`agent/conversation.json`](agent/conversation.json)。
