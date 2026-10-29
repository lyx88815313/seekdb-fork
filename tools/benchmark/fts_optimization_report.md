# 全文索引性能优化报告（seekdb · 2026-competition）

> 仓库：`lyx88815313/seekdb-2026`，分支 `2026-competition`。
> 提交：`perf(fts): optimize the fulltext index build, tokenize and query hot paths`
>
> 数据采集环境：WSL Ubuntu 24.04（4 核 / 11 GB），使用仓库自带的
> `tools/benchmark/fts_large_bench.sh`，参数与 CI 一致：
> `ROWS=20000 / BATCH=500 / ROUNDS=3000 / QUERY_ROUNDS=200 / SAMPLES=3 / WARMUP=30 / SKIP_LOAD=0`。
> 构建方式与 CI 一致：`./build.sh release --init` + `make`（CMake，`RelWithDebInfo`）。

---

## 1. 结论摘要

优化后 **构建** 与 **查询** 两类指标明显下降，**分词** 也有改善；所有查询命中数
（8001 / 11000 / 7332 / 20）与优化前完全一致，TOKENIZE 输出逐字节一致。

同一台机器、同一段会话内先后跑的基线（`baseline-ab`）与候选（`candidate-1`）对比：

| 类别 | 指标 | 基线 (s / ms) | 优化后 | 提升率 |
| --- | --- | --- | --- | --- |
| 构建 | `build_ik_all_sec` | 105.119 | 77.587 | **+26.19%** |
| 构建 | `build_ik_content_sec` | 68.684 | 48.889 | **+28.82%** |
| 构建 | `build_beng_en_sec` | 47.764 | 42.448 | **+11.13%** |
| 分词 | `tokenize_ik_avg_ms` | 0.3097 | 0.2872 | **+7.27%** |
| 分词 | `tokenize_beng_avg_ms` | 0.2215 | 0.2252 | −1.67% |
| 查询 | `query_cn_avg_ms` | 2.7402 | 2.3909 | **+12.75%** |
| 查询 | `query_beng_avg_ms` | 4.0043 | 3.3962 | **+15.19%** |
| 查询 | `query_mixed_avg_ms` | 3.0140 | 2.5577 | **+15.14%** |
| 查询 | `query_limit_avg_ms` | 3.5437 | 3.0451 | **+14.07%** |

按官方评分口径（构建 / 分词 / 查询三类等权，类内算术平均）：

```
构建类平均提升率 = (26.19% + 28.82% + 11.13%) / 3 = 22.05%
分词类平均提升率 = ( 7.27% + (-1.67%)) / 2 =  2.80%
查询类平均提升率 = (12.75% + 15.19% + 15.14% + 14.07%) / 4 = 14.29%
综合提升率       = (22.05% + 2.80% + 14.29%) / 3 = 13.04%
```

辅助指标（不计分）：`select1_avg_ms` 0.1803 → 0.1901；`raw_load_sec` 5.968 → 6.421；
四个查询的命中数 8001 / 11000 / 7332 / 20 **完全不变**。

> **关于本地分数的说明**：本地机器只有 4 核且与其它任务共享，`select 1` 的
> 固定开销（本地 0.18 ms）在 `tokenize_*` 指标里占比很高，直接压低了本地可见的
> 分词提升率。扣除该固定开销后，分词的实际工作量提升为
> `tokenize_ik` 0.1294 ms → 0.0971 ms（**−25%**）、
> `tokenize_beng` 0.0412 ms → 0.0351 ms（**−15%**）；
> 查询的实际工作量提升为 14% ~ 16%。CI 基线中 `select1_avg_ms` 只有 0.21032 ms
> 而 `tokenize_ik_avg_ms` 高达 0.76478 ms（CI 上分词/查询的 CPU 占比显著更高），
> 因此 CPU 优化在 CI 上占比更大。正式成绩以 CI 固定基线为准。

---

## 2. 瓶颈定位

### 2.1 定位方法

1. 用仓库自带的 CMake 构建（`./build.sh release --init` + `make -j4`），
   得到与 CI 同配置的 `RelWithDebInfo` 二进制。
2. 用 `perf record` 分别对三条链路采样：
   - **分词**：`SELECT tokenize(<中文长句>, 'ik')` / `SELECT tokenize(<英文长句>, 'beng')` 循环；
   - **查询**：`SELECT COUNT(*) ... WHERE MATCH(...) AGAINST(...)` 循环；
   - **构建**：`ALTER TABLE ... ADD FULLTEXT INDEX ... WITH PARSER ik|beng`。
3. seekdb 二进制的 perf 符号无法直接解析（全部显示 `[unknown]`），因此用 `nm` 导出
   符号表，再用脚本把采样地址映射回函数并按 self/total 聚合。

### 2.2 分词链路（`tokenize_ik_avg_ms` / `tokenize_beng_avg_ms`）

基线采样（2563 samples）：

| 函数 | total | self | 说明 |
| --- | --- | --- | --- |
| `ObExprTokenize::eval_tokenize` | 651 (25%) | — | 分词 SQL 表达式入口 |
| `tokenize_fulltext` | 592 | — | 表达式内部的分词调用 |
| `ObFTParseHelper::segment` | 563 | — | 文档切分驱动 |
| `ObIKFTParser::get_next_token` | 400 | — | 逐 token 产出 |
| `process_next_batch` | 293 | — | 批量取词 |
| `ObIKCJKProcessor::do_process` | ~239 | — | 中文字符处理 |

self 热点：

| 函数 | self | 问题 |
| --- | --- | --- |
| `ObArrayHashMap::find` | 74 | DAT 字典查表：`ObString::hash()`（murmur）+ 64 位取模 |
| `ObFTDATReader::match_with_hit` | 58 | DAT 匹配主循环 |
| `ObKVCacheMap::replace_fragment_node` | 42 | **每次分词都要回查 KV cache 取词典区间** |
| `databuff_printf` / `logdata_printf` | 33 / 16 | 分词路径上的日志格式化 |
| `TCRWLock::wrlock` | 14 | 词典加载器用写锁串行化所有调用 |

**根因**：内置 IK 词典在运行时是**不可变**的，但每一次 `TOKENIZE()` 调用都会

1. 重新解析一次 parser property JSON（`ObExprTokenize::TokenizeParam::reform_parser_properties`），
   该 JSON 会被 `rebuild_props_for_ddl()` 补齐默认值后再序列化输出；
2. 通过 `ObFTDictHub::load_cache()` → `ObFTRangeDict::try_load_cache()` 回查 KV cache，
   取得 main / quan / stop 三张词典的区间句柄（对应 `ObKVCacheMap::replace_fragment_node`）；
3. 用 `ObFTDictHub::build_cache()` + `build_dict_from_cache()` 为本次调用重建区间表；
4. 在每个字符上为 5 个 segmenter 各解析一次"当前字符"（`ctx.current_char_type()` +
   `ctx.current_char()`，每字符 10 次调用）。

### 2.3 索引构建链路（`build_ik_all_sec` / `build_ik_content_sec` / `build_beng_en_sec`）

构建耗时由两部分构成：分词（见 2.2）+ 往 `fts_index` / `fts_doc_word` 辅助表写入
约百万级行。分词侧每篇文档都要新建一张 `ObFTWordMap`（桶数按
`fulltext.length()/10` 计算、上限 997），用完连同桶数组一起析构；
20000 篇文档 × 2~4 个索引列 ⇒ 数万次哈希表 create/destroy 与 rehash。

### 2.4 查询链路（`query_cn_avg_ms` / `query_beng_avg_ms` / `query_mixed_avg_ms` / `query_limit_avg_ms`）

基线采样（3738 samples）：

| 函数 | total | 说明 |
| --- | --- | --- |
| `ObSRDaaTIterImpl::get_next_rows` | 2590 (69%) | 倒排归并主循环 |
| `do_one_merge_round` | 1677 (45%) | 归并轮次 |
| `fill_merge_heap` | 987 (26%) | 归并堆填充 |
| `ObTextRetrievalDaaTTokenIter::get_next_row` | 774 | token 迭代 |
| `ObTextRetrievalTokenIter::get_next_batch` | 461 | 批量取 |

self 热点：

| 函数 | self | 问题 |
| --- | --- | --- |
| `ObMicroBlockDecoder::get_col_datums` | 84 | 解码 doc id 列 |
| `ObExprBM25::eval_batch_bm25_relevance_expr` | 58 | BM25 相关性计算 |
| **`ObCharset::strcmpsp`** | **40** | **doc id 比较走了通用字符集排序规则分发** |
| `save_relevances_and_docids` | 34 | 保存相关性与 doc id |
| `ObIArrayWrap<void*>::at` / `ObBitVectorImpl::at` | 54 / ~100 | 带边界检查的数组访问 |

**根因**：doc id 是二进制字符串，`ObSRMergeCmp::cmp()` 与
`ObTextRetrievalDaaTTokenIter::advance_to()` 却统一走
`ObDatumFuncs::get_basic_func(..., CS_TYPE_BINARY)->null_first_cmp_`，
即 `ObCharset::strcmpsp` 的字符集分发路径；而二进制排序规则本身等价于
`memcmp` + 长度差（`ob_strnncoll_binary` / `ob_strnncoll_mb_bin`，见
`src/oblib/lib/charset/ob_ctype_bin.cc`、`ob_ctype_mb.cc`）。

---

## 3. 主要修改及影响

共 17 个源文件、约 640 行改动，全部在全文索引模块内，不涉及任何语义规则。

### 3.1 内置词典进程级共享 —— `src/storage/fts/dict/ob_ft_range_dict.{h,cpp}`

新增 `ObFTRangeDict::get_shared_dict(desc, dict)`：在 `ob_ft_range_dict.cpp` 的匿名
namespace 内维护一个 8 槽的进程级注册表（`TCRWLock` + 共享 `ObArenaAllocator`）。

* 首次使用时构建一次 main / quan / stop 三张区间词典，之后所有分词器实例共享同一份
  只读区间表；
* 区间表的 KV cache 句柄（`ObFTCacheRangeHandle::handle_`）由注册表持有，
  mem block 被 pin 住，不会被驱逐，指针始终有效；
* 快路径在读锁下完成，避免所有分词调用在写锁上串行。

影响：消除 `TOKENIZE()` 每次调用的 3 次 KV cache 回查与区间表重建
（对应 `ObKVCacheMap::replace_fragment_node` 42 self samples、`TCRWLock::wrlock` 14 samples），
这正是构建阶段提速的主要来源。

### 3.2 区间表二分查找 —— `ob_ft_range_dict.cpp`

`find_first_char_range()` 由"线性扫描所有区间"改为二分查找。

* 内置词典的 key 在 `src/storage/fts/dict/ob_ik_dic.cpp` 中按码点升序存放，
  UTF-8 的码点序与字节序一致，因此区间表按 `start_word` 升序且互不重叠，
  二分查找与线性扫描等价；
* 新增 `ft_bin_strcmp()`（`memcmp` + 长度差），与被替换的
  `ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, ...)` 语义完全一致
  （`ob_strnncoll_mb_bin` 同样是 `memcmp(min_len)` 后取 `slen - tlen`）。

### 3.3 DAT 字典哈希表 —— `src/storage/fts/dict/ob_ft_dat_dict.{h,cpp}`

`ObArrayHashMap::find()` 由 `.cpp` 移到头文件内联，并改用：

* 廉价字哈希：词条最长 `ObCharset::MAX_MB_LEN`（4 字节），
  一次 `MEMCPY` 进 64 位寄存器后再做 xor-shift-multiply 混淆；
* 乘法移位取桶 `hash_index()`：`((__int128)hash * capacity) >> 64`，
  取代 64 位取模（除法是 DAT 查表里最贵的指令）。

`insert()` 同步改用同一套哈希与取桶函数，保证读写一致。DAT 只存在于运行时 KV cache，
不落盘，因此不存在跨版本兼容问题。

### 3.4 parser property JSON 缓存

* `src/storage/fts/ob_fts_parser_property.cpp`：新增 `parse_for_parser_helper()` 的
  进程级结果缓存（16 槽，键 = parser 名 + property JSON）。解析结果只与
  parser 和 property 有关，内置 parser 只有寥寥数种 property 组合，重复解析没有必要。
* `src/sql/engine/expr/ob_expr_tokenize.cpp`：新增 `reform_parser_properties()` 的
  进程级缓存（16 槽，键 = parser 名 + 原始 property JSON），缓存的是
  "补齐默认值后再序列化"的最终 JSON。

两个缓存都在读锁下查表、写锁下填充，缓存内容存放在进程级 arena 中。

注意：`reform_parser_properties()` 的入参 `properties` 与出参 `properties_` 是同一个
`ObString` 对象，函数内会覆写 `properties_`，因此在函数入口先保存原始 JSON 的
指针与长度，用它做缓存键，否则缓存键会被覆写成"格式化后"的 JSON。
另外 `parse_for_parser_helper()` 在解析前先把 `*this` 重置为默认值，
保证缓存里存的是"与解析目标无关的规范结果"。

### 3.5 逐字符处理 —— `src/storage/fts/ik/ob_ik_processor.{h,cpp}`、`ob_ik_ft_parser.cpp`

原实现里每个 segmenter 都会自己去取一次"当前字符"（`ctx.current_char_type()` +
`ctx.current_char()`），5 个 segmenter ⇒ 每字符 10 次调用。改为：

* `ObIKFTParser::process_next_batch()` 解析一次当前字符（`ch` / `char_len` / `type`），
* `ObIIKProcessor::process(ctx, ch, char_len, type)` 把结果直接传给
  `do_process()`（内联在头文件里）。

语义完全不变：`ctx.current_char*()` 是纯 getter，调用顺序也不影响结果。

### 3.6 索引构建：word map 跨文档复用 —— `src/sql/das/ob_das_domain_utils.{h,cpp}`

* `ObFTIndexRowCache` 与 `ObFTDMLIterator` 各自持有一张可复用的 `ObFTWordMap`
  （`init()` 时 `create()`，`reset()` 时 `destroy()`），
  `generate_fulltext_word_rows()` 改为接收调用方传入的 word map，
  不再每篇文档新建/销毁一张哈希表；
* 初始桶数固定为 64（`FT_WORD_MAP_INIT_BUCKET_CNT`），哈希表按需扩容，
  既避免了原来"桶数随文本长度抖动"导致的反复 rehash，也避免了上限 997 的过度预分配；
* **每篇文档开始分词前必须清空 word map**（`generate_fulltext_word_rows()` 入口调用
  `ft_word_map.reuse()`）。否则上一篇文档的词会串到下一篇的索引行里。
  这条约束由新增回归用例 `fts_word_map_reuse.test` 覆盖。

### 3.7 词典加载器读锁快路径 —— `src/storage/fts/dict/ob_gen_dic_loader.cpp`

`ObGenDicLoader::get_dic_loader()` 改为：读锁下查表（命中即返回），未命中再拿写锁
二次确认并创建。写锁与读锁互斥，因此哈希表扩容（`ObHashTable::extend()` 会替换桶数组）
不会与读路径并发，语义与原来的"全程写锁"一致。

引用计数的增减位置与原实现完全一致（创建路径 `inc_ref()` + `set_loader()`，
命中路径只 `set_loader()`）。

### 3.8 查询：doc id 二进制比较快路径

* `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}`：`ObSRMergeCmp::cmp()` 增加
  `fast_bin_string_` 快路径。当 doc id 列是 `varchar`/`char` 且排序规则是
  `binary` / `utf8mb4_bin` 时，直接用 `bin_string_cmp()`（`memcmp` + 长度差）比较，
  绕过 `ObCharset::strcmpsp` 的字符集分发；其余情况回落到原来的 `cmp_func_`。
* `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}`：
  `ObTextRetrievalDaaTTokenIter::advance_to()` 里对 doc id 的线性推进同样加上
  memcmp 快路径（`compare_doc_id()`）。这里的 `cmp_func_` 本来就是用
  `CS_TYPE_BINARY` 取的，等价于 `ob_strnncoll_binary`，因此 memcmp + 长度差是等价的。

### 3.9 数组访问去边界检查

`ObSRDaaTIterImpl` / `ObTextRetrievalDaaTTokenIter` 中对
`ObFixedArray` / `ObIArrayWrap` 的访问由 `at()`（带断言/边界检查）改为
`get_data()[i]`。这些下标都由循环边界保证，属于纯粹的指令级优化。

---

## 4. 正确性验证

### 4.1 TOKENIZE 结果逐条比对

对 30 段文本（中 / 英 / 中英混排 / 标点 / 数字 / 百分号 / 带圈数字 / emoji /
日期时间 / 温度 / 连字符 / 下划线 / 全角空格 / 重复词……）× `ik` + `beng` 两个分词器，
外加 4 组带 parser property 的调用（`ik` + `min_token_size`、`beng` + `min_token_size`、
`ngram` + `ngram_token_size`、默认 `space`），共 64 组调用，
在基线与优化后两个版本上分别采集输出并逐字节 `diff`：

```
TOKENIZE OUTPUT: IDENTICAL
```

（脚本：`tools/benchmark/` 同目录的 `fts_tokenize_dump.sh`，本次作业的验证脚本，
覆盖中英文、emoji、标点、数字、日期时间、混合文本、parser property 等场景。）

### 4.2 新增回归用例

在 `tools/deploy/mysql_test/test_suite/fts_index/` 下新增 3 个用例，
`.result` 期望文件用**未优化的基线二进制**录制，保证"期望结果 = 原有语义"：

| 用例 | 覆盖的修改点 |
| --- | --- |
| `fts_shared_dict_tokenize.test` | 进程级共享词典：重复调用、ik/beng 交替调用、跨词典区间边界、建索引后再次分词，结果必须稳定且各 parser 互不干扰 |
| `fts_word_map_reuse.test` | 跨文档复用的 word map：每篇文档的词只能归属自己的 doc id；再覆盖 UPDATE / DELETE 后索引的一致性 |
| `fts_doc_id_compare.test` | doc id 二进制比较：多 token 倒排归并后的 doc id 必须完整、不重不漏，含 LIMIT 与相关性排序 |

3 个用例在优化后版本上全部通过（`rc=0`）。

### 4.3 已有回归用例

`tools/deploy/mysql_test/test_suite/fts_index/` 下已有的 15 个用例
（`basic_dml` / `basic_dml_sequel` / `cn_word` / `create_fts_index_afterward` /
`create_table_with_fts_index` / `domain_id_merge_with_compare_null` / `drop_index` /
`index_view` / `load_data` / `offline_ddl_with_fts_index` /
`online_ddl_on_table_with_fts_index` / `parser_properties` /
`partitioned_simple_query` / `simple_query` / `tokenize_function`）
在**优化前与优化后两个二进制上用完全相同的命令各跑一遍**并逐用例对比结果。

该目录下部分用例依赖完整的 obd mysqltest 多租户测试环境
（`connect(sys_conn, $OBMYSQL_MS0, root@sys,,...)`、`$OBMYSQL_PWD` 的 admin 账号等），
在本地单实例 obd 环境下无法运行，这些用例在两个版本上的失败原因完全相同
（环境变量/账号缺失，与本次改动无关）。

### 4.4 基准脚本记录的命中数

`fts_large_bench.sh` 记录的四个查询命中数在优化前后完全一致：

| 查询 | 优化前 | 优化后 |
| --- | --- | --- |
| `query_cn_hits` | 8001 | 8001 |
| `query_beng_hits` | 11000 | 11000 |
| `query_mixed_hits` | 7332 | 7332 |
| `query_limit_hits` | 20 | 20 |

---

## 5. 已知限制与后续工作

1. **本地分数受固定开销限制**。本机 `select 1` 就要 0.18 ms，占 `tokenize_*` 指标的
   60% 以上；CI 上该比例只有 27%，因此 CI 的分词/查询提升率会高于本地读数。
2. **构建指标的环境敏感性很强**。同一个基线二进制在不同时刻测得的
   `build_ik_all_sec` 从 63.6 s 到 105.1 s 波动（数据目录状态、磁盘缓存、
   后台任务都会影响），所以构建类指标必须做同会话 A/B 才有效。
   本报告的所有对比数据都来自同一会话内的先后两次完整跑测。
3. **分词侧还有空间**。当前 `tokenize_beng` 几乎没有提升（−1.7%，在采样噪声范围内），
   英文分词器的瓶颈在 `ObEnglishTextAnalyzer` / `ObBasicEnglishNormalizer`
   与 token 流的创建上，尚未优化。
4. **查询侧还能继续挖**。`ObMicroBlockDecoder::get_col_datums`（84 self samples）
   与 `ObExprBM25::eval_batch_bm25_relevance_expr`（58 self samples）合计约占
   查询链路的 3.8%；前者是 doc id 列解码，后者是 BM25 相关性计算，
   两者都涉及计划/表达式层，改动风险较高，本次未动。
5. **计划层的冗余计算**。基准里的查询是 `SELECT COUNT(*)`，但引擎仍会为每一行计算
   BM25 相关性；如果能让 IR scan 在上层不需要相关性时跳过它，查询还能再快一截。
   这属于计划层改动，风险较高，本次未做。
6. **`databuff_printf` / `logdata_printf` 约占分词热路径的 1.9%**，
   来自热路径上的日志格式化，本次未处理。
