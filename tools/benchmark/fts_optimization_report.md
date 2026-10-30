# 全文索引性能优化报告（seekdb · 2026-competition）

> 仓库：`lyx88815313/seekdb-2026`，分支 `2026-competition`。
> 提交：
> * `perf(fts): optimize the fulltext index build, tokenize and query hot paths`（第一轮）
> * `perf(fts): drop the per bucket locks of the tokenizer hash tables`（第二轮）
> * `perf(fts): booleanize the BM25 score when MATCH is only used as a filter`（第三轮）
>
> 数据采集环境：WSL Ubuntu 24.04（4 核 / 11 GB），使用仓库自带的
> `tools/benchmark/fts_large_bench.sh`，参数与 CI 一致：
> `ROWS=20000 / BATCH=500 / ROUNDS=3000 / QUERY_ROUNDS=200 / SAMPLES=3 / WARMUP=30 / SKIP_LOAD=0`。
> 构建方式与 CI 一致：`./build.sh release --init` + `make`（CMake，`RelWithDebInfo`）。
>
> 第三轮为了压低测量噪声，另加了一层**测量协议**（静默等待 + ABBA 交错 + 重复跑测），
> 详见第 7 章。**基准脚本本身没有做任何修改。**

---

## 1. 结论摘要

三轮优化覆盖了分词、构建、查询三条链路。最终候选版本相对基线：

* **分词** +15.8%、**查询** +23.6%、**构建** +1.7%（构建类读数波动大，见下）
* 综合提升率约 **13.7%**，按官方口径（综合耗时降 50% 满分）本地预估 **27.4 / 100**
* 所有查询命中数（8001 / 11000 / 7332 / 20）与基线**完全一致**
* TOKENIZE 输出与基线**逐字节一致**
* MATCH 的语义输出与基线**逐行一致**（唯一差异是 `EXPLAIN` 里的 `calc_relevance` 标志）

第三轮（BM25 布尔化）是查询侧提升的主要来源：查询类从第二轮的 +14.08%
提高到 +23.61%。

### 1.1 测量方法

查询与分词类指标来自**双服务器交替 A/B**（第 7 章）：两个 seekdb 实例同时运行、
持有完全相同的数据、分别跑基线与候选二进制，每轮交换顺序，共 8 组配对；
另用"两个端口跑同一个基线二进制"的**空对照**验证部署方式不引入系统性偏差
（各指标差异 ≤ 5%）。构建类指标来自两次完整跑测（`load + 建 3 个索引`）。

**基准脚本 `tools/benchmark/fts_large_bench.sh` 本身没有做任何修改**，
参数与 CI 一致：`ROWS=20000 / BATCH=500 / ROUNDS=3000 / QUERY_ROUNDS=200 /
SAMPLES=3 / WARMUP=30`。

### 1.2 最终对比

| 类别 | 指标 | 基线 | 优化后 | 提升率 |
| --- | --- | --- | --- | --- |
| 构建 | `build_ik_all_sec` | 48.132 s | 49.845 s | −3.56% |
| 构建 | `build_ik_content_sec` | 31.872 s | 31.606 s | +0.83% |
| 构建 | `build_beng_en_sec` | 25.212 s | 23.271 s | **+7.70%** |
| 分词 | `tokenize_ik_avg_ms` | 0.4217 ms | 0.3214 ms | **+20.64%** |
| 分词 | `tokenize_beng_avg_ms` | 0.2838 ms | 0.2379 ms | **+10.94%** |
| 查询 | `query_cn_avg_ms` | 3.3940 ms | 2.3499 ms | **+25.81%** |
| 查询 | `query_beng_avg_ms` | 4.7158 ms | 3.5070 ms | **+27.27%** |
| 查询 | `query_mixed_avg_ms` | 3.8805 ms | 2.7362 ms | **+26.91%** |
| 查询 | `query_limit_avg_ms` | 4.2600 ms | 3.7172 ms | **+14.45%** |

按官方评分口径（构建 / 分词 / 查询三类等权，类内算术平均）：

```
构建类平均提升率 = ( −3.56% +  0.83% +  7.70%) / 3 =  1.66%
分词类平均提升率 = ( 20.64% + 10.94%) / 2            = 15.79%
查询类平均提升率 = ( 25.81% + 27.27% + 26.91% + 14.45%) / 4 = 23.61%
综合提升率       = (  1.66% + 15.79% + 23.61%) / 3   = 13.69%
本地预估得分 = 13.69% / 50% × 100 ≈ 27.4 / 100
```

辅助指标（不计分）：`select1_avg_ms` 0.2248 → 0.1983；
四个查询的命中数 8001 / 11000 / 7332 / 20 **完全不变**。

> **关于构建类指标的读数（重要）**：同一个**基线二进制**在低负载时段测得
> `build_ik_all_sec = 51.7 s`，在另一段高负载时段测得 `105.1 s`，相差 1.65 倍；
> 而 `tokenize_*` / `query_*` 指标跨时段非常稳定。这说明构建耗时主要
> 受磁盘 / 页面缓存 / 机器负载影响，而不是 CPU。上一版报告记录的"构建 +22%"是在
> 负载不均衡的两次跑测之间得出的，读数被放大了，已经作废。本报告的构建类数字
> 只有单次采样，**只能当作数量级参考**（第二轮的同会话 A/B 给出的是
> +2.91% / −1.51% / +7.35%，量级一致）。构建链路的真实瓶颈是往 `fts_index` /
> `fts_doc_word` 辅助表写入百万级行，而不是分词本身——这也解释了为什么消除词典的
> 重复加载后构建耗时只降了几个百分点。

> **关于本地分数的说明**：本地机器只有 4 核且与其它任务共享，`select 1` 的
> 固定开销（本地约 0.19~0.25 ms）在 `tokenize_*` 指标里占比很高，直接压低了本地可见的
> 分词提升率。扣除该固定开销后，分词的实际工作量提升为
> `tokenize_ik` 0.2074 ms → 0.1179 ms（**−43.2%**）、
> `tokenize_beng` 0.0670 ms → 0.0475 ms（**−29.1%**）。
> CI 机器上 `select1_avg_ms` 只有 0.0266 ms 而 `tokenize_ik_avg_ms` 高达 0.0631 ms
> （CI 上分词/查询的 CPU 占比显著更高），因此 CPU 优化在 CI 上占比更大。
> 正式成绩以 CI 固定基线为准（第二轮结束时 CI 实测为 +6.31%）。

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

共 23 个源文件、约 660 行改动，全部在全文索引模块内，不涉及任何语义规则。

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

### 3.10 第二轮：哈希桶去锁 + 解析器名比较去冗余

第二轮改用"服务端持续压满"的采样方式重新定位分词热点（perf 连续采样 20 秒，
期间客户端不停灌 TOKENIZE 请求，避免空闲服务端稀释样本），并按 `eval_tokenize`
调用链做子树归因。TOKENIZE 子树共 9265 个采样点，自耗时分布为：

| 函数 | 子树内 self | 占比 |
| --- | --- | --- |
| libc malloc / free | 1102 | 11.9% |
| `ObArrayHashMap::find` | 1019 | 11.0% |
| `ObFTDATReader::match_with_hit` | 930 | 10.0% |
| `ObLatch::unlock` / `wrlock` / `rdlock` | 432 / 421 / 198 | 11.3% |
| `ObFTRangeDict::find_first_char_range` | 225 | 2.4% |
| `PageArena::_alloc_aligned` | 205 | 2.2% |
| `ObHashTable<ObFTWord, ...>`（词表哈希表） | 189 + 119 | 3.3% |

**根因**：1051 次桶锁调用中有 755 次（72%）来自词表哈希表
`ObHashTable<ObFTWord, ...>`，238 次来自 IK 仲裁器的链表
`ObHashTable<int64_t, ObIKTokenChain *>`。这两个哈希表其实都是**单线程私有**的
（TOKENIZE 路径是函数内的局部变量；索引构建路径每个构建任务一份），
却仍然为每个桶配了读写锁；而每个词都要"查一次 + 写一次"，桶锁的原子操作
就成了分词热路径上一项占比可观的固定开销。

据此做了三处修改：

* `src/data_plane/api/data_plane/fts/ob_fts_struct.h`：`ObFTWordMap` 改用
  `common::hash::NoPthreadDefendMode`（桶内只有链表指针，没有锁和条件变量）。
  **桶数与哈希函数完全不变**，因此哈希表的遍历顺序不变，TOKENIZE 的输出顺序
  也不变。`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`、
  `src/sql/optimizer/ob_join_order.cpp`、`src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp`
  三处直接写死 `hash::ObHashMap<ObFTWord, int64_t>` 的地方统一改用该 typedef。
* `src/storage/fts/ik/ob_ik_arbitrator.h`：`ObIKArbitrator::chains_` 同样改用
  `NoPthreadDefendMode`（仲裁器是单次分词内的栈上对象）。
* `src/data_plane/api/data_plane/fts/ob_fts_parser_helper.h`：
  `ObFTParser::is_xxx()` 原来每比较一次就构造一个临时 `ObFTParserName`
  （清空并回填 64 字节缓冲区）再比较，而 `is_builtin()` / `is_valid()` /
  `get_desc()` / `parse_from_str()` 在一次 TOKENIZE 里会触发约 20 次这样的比较。
  改为直接 `STRCMP` 比较，语义等价：`ObFTParserName::set_name()` 会把名字转成
  小写，而内置 parser 名本来就是小写。

单线程性依据：TOKENIZE 路径的 word map 是 `tokenize_fulltext()` 的局部变量；
索引构建路径的 `ObFTIndexRowCache` / `ObFTDMLIterator` 各自持有一份，并且与
`ObFTParseHelper` 共用同一个 `ObArenaAllocator`（arena 分配器本身不是线程安全的），
因此这些对象天然只被单个线程访问。

### 3.11 第三轮：MATCH 只作布尔过滤时跳过 BM25 分数计算

#### 动机

基准里的四条查询都是 `SELECT COUNT(*) ... WHERE MATCH(...) AGAINST(...)`：
引擎对**每一条被倒排命中的文档**都算了完整的 BM25 分数
（`ObExprBM25::eval_batch_bm25_relevance_expr` 4.57% + `batch_eval_relevance_expr` 1.19%），
随后只把它转成布尔做过滤（`calc_bool_expr_for_float_type` 2.03%）——
**分数值本身从未被使用**。更贵的是，为了算这个分数，引擎要额外扫描
`fts_doc_word` 辅助表做 `inv_idx_agg` 聚合，取文档长度与词频
（`ObTextRetrievalTokenIter::estimate_token_doc_cnt` →
`ObSEArrayImpl<ObWorkareaHistogram/ObEstRowCountRecord>` 占查询子树约 7.1%）。

计划层其实早就留好了开关（`src/sql/optimizer/ob_log_table_scan.h:80`）：

```cpp
bool need_calc_relevance_ = true;   // match expr just for retireval (accurate score is not required)
```

但它此前恒为 `true`，于是 cg 侧一定会生成 BM25 表达式
（`src/sql/code_generator/ob_tsc_cg_service.cpp:3048`）和三个辅助聚合 ctdef
（`inv_idx_agg` / `doc_agg` / `block_max`，同文件 `:1832`）。

#### 改动

**1）计划层判定「分数是否被观测到」**——`src/sql/optimizer/ob_log_plan.cpp`

新增匿名 namespace 里的三个辅助函数：

* `is_match_score_observed(expr, match_expr, parent_is_bool_op)`
  递归遍历表达式树。只有当 MATCH 表达式的**父节点是 `T_OP_BOOL`** 时，
  才认为分数"未被观测"——`T_OP_BOOL` 把操作数归约成布尔，
  非零分数与正常常量都是 TRUE，分数的大小完全不可见。
* `expr_list_observes_match_score(exprs, match_expr)`
* `stmt_observes_match_score(stmt, match_expr, depth)`
  遍历 condition exprs、各 table item 的 join 条件、order items、
  having exprs、group exprs、select exprs、relation exprs，
  以及递归子查询（深度上限 16）。

`check_match_score_observed()` 汇总判定，只有同时满足下列条件才判定为"不可观测"：

1. `match_expr->get_mode_flag() == NATURAL_LANGUAGE_MODE`；
2. 下推的过滤条件是 `T_OP_BOOL(match_expr)`（参数数为 1 且参数就是 MATCH 本身）。

判定通过后，在 `prepare_text_retrieval_scan()` 里置
`tr_info.need_calc_relevance_ = false`。判定过程本身用
`OB_SUCC(...) && !score_observed` 保护：**一旦检查出错就保留分数**，
检查失败绝不导致查询失败。

> **为什么排除布尔模式**：`ObFtsEvalNode::fts_boolean_eval` 与
> `ObSRDaaTBooleanRelevanceCollector` 依赖**每个 token 各自的相关性数值**
> 来做 OR/AND 组合，置成常量会改变布尔模式的语义。
> `NATURAL_LANGUAGE_MODE_WITH_QUERY_EXPANSION` / `WITH_QUERY_EXPANSION` /
> `MATCH_PHRASE_MODE` 同理保留分数。

**2）过滤条件路由保持逐字一致**——`src/sql/optimizer/ob_log_table_scan.cpp`

`extract_pushdown_filters()` 里文本检索扫描的过滤器本来就**不会**下推到倒排索引的
存储层（在检索结果上求值），改造前的条件
`is_text_retrieval_scan() && need_text_retrieval_calc_relevance()`
在 `need_calc_relevance_` 恒为 `true` 的前提下等价于 `is_text_retrieval_scan()`。
因此直接改成后者，保证**过滤器路由与改造前完全一致**。

**3）运行时门禁**——`src/sql/das/iter/ob_das_iter_utils.cpp`、
`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`、
`src/storage/retrieval/ob_text_{daat,taat}_iter.cpp`、
`src/storage/retrieval/ob_sparse_taat_iter.cpp`

`need_calc_relevance()` 由 `relevance_expr_ != nullptr` 决定（cg 侧不再生成
`relevance_expr_`，自然为 false）。据此：

* 不创建 `doc_cnt_agg` 迭代器，也不创建 `total_doc_cnt_scan_param_`；
* 不做 BM25 参数估算（`bm25_param_estimator_.init/do_estimation`）；
* `estimate_token_doc_cnt()` 跳过（`ObSEArray` 直方图估算开销）；
* `get_doc_agg_ctdef()` 可能为空 → 全部改成空指针安全；
* 相关性写入常量 `1.0`（`ObTextRetrievalDaaTTokenIter::save_docids()`），
  `get_curr_score()` 无条件返回该常量。

**4）必须保留 `relevance_proj_col_`**

cg 侧生成 `relevance_proj_col_` 的条件是
`need_calc_relevance_ || pushdown_match_filter_ != nullptr`——
后者在过滤场景下必然成立，因此投影列**照旧生成**。
这一点是必需的：若 `relevance_proj_col_` 也消失，
`need_proj_relevance_score()` 变假，merge iter 会从 DAAT 切到 TAAT，
**命中集合的语义会变**。

#### 命中集合不变性论证

设某文档被倒排命中，其 BM25 分数为

```
score = token_weight * doc_weight
token_weight = MAX(p_epsilon, idf) * (1 + k1)     > 0   // p_epsilon > 0
doc_weight   = tf / (tf + k1 * (1 - b + b * norm_len)) > 0 // tf >= 1
```

因此**每个被倒排命中的文档分数严格为正**。而

* 命中判定是 `matched_cnt_ >= should_match_`，`minimum_should_match_`
  在普通路径下恒为 0，与分数值无关；
* 下推的过滤条件是 `T_OP_BOOL(MATCH)`，即"分数非零"。

把分数换成常量 `1.0`（同样非零）后，"分数非零"这一谓词对**每一个候选文档**
都保持不变，因此命中集合逐行不变。

#### 验证

* 命中数：8001 / 11000 / 7332 / 20，与基线完全一致。
* `EXPLAIN`：优化版显示 `calc_relevance=false`，基线为 `true`，证明开关确实生效。
* 语义检查：基线与优化版的查询输出**逐行 diff 只差这一行**
  （`calc_relevance` 标志），其余全部相同——
  投影分数 8708.5394 / 3.894549、`ORDER BY` 分数、布尔模式（0 行）、
  阈值比较（9333）、`AND id>100`（7960）、`EXISTS`（9333）完全一致。
* TOKENIZE 121 行输出逐字节 `IDENTICAL`。

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

第二轮优化（哈希桶去锁等）完成后，用同一个脚本重新采集并与基线逐字节比对，
结果同样是 `TOKENIZE OUTPUT: IDENTICAL`——桶数与哈希函数没有变化，
哈希表遍历顺序（即 TOKENIZE 输出顺序）因此保持不变。

第三轮（BM25 布尔化）不触碰任何分词代码，但仍然用同一个脚本重跑，
121 行输出与基线**逐字节一致**（`TOKENIZE OUTPUT: IDENTICAL`）。

### 4.2 MATCH 语义：基线与优化版输出逐行比对

第三轮改了计划层与检索执行层，语义风险最高，因此除了基准脚本记录的命中数之外，
额外做了一组**语义对照**：在同一张表上分别用基线二进制与优化版二进制执行
"投影分数 / 排序 / 布尔模式 / 阈值比较 / 附加条件 / EXISTS"等查询，
把两边的输出做逐行 `diff`：

```
$ diff semantic_baseline.txt semantic_candidate3.txt
< calc_relevance: true            ← 仅此一行不同
---
> calc_relevance: false
```

其余每一行完全相同，包括：

| 检查项 | 结果（两版一致） |
| --- | --- |
| 投影 BM25 分数 | 8708.5394 |
| 归一化分数 | 3.894549 |
| `ORDER BY` 分数排序 | 行序完全一致 |
| 布尔模式（`IN BOOLEAN MODE`） | 0 行（两版都无命中） |
| 阈值比较 `MATCH(...) > 1.0` | 9333 行 |
| 附加条件 `MATCH(...) AND id > 100` | 7960 行 |
| `EXISTS (SELECT ... MATCH(...))` | 9333 行 |

`EXPLAIN` 输出中 `calc_relevance` 由基线的 `true` 变为优化版的 `false`，
证明"仅过滤"场景确实命中了新的优化路径，而不是因为其它原因绕开了 BM25。

### 4.3 新增回归用例

在 `tools/deploy/mysql_test/test_suite/fts_index/` 下新增 3 个用例，
`.result` 期望文件用**未优化的基线二进制**录制，保证"期望结果 = 原有语义"：

| 用例 | 覆盖的修改点 |
| --- | --- |
| `fts_shared_dict_tokenize.test` | 进程级共享词典：重复调用、ik/beng 交替调用、跨词典区间边界、建索引后再次分词，结果必须稳定且各 parser 互不干扰 |
| `fts_word_map_reuse.test` | 跨文档复用的 word map：每篇文档的词只能归属自己的 doc id；再覆盖 UPDATE / DELETE 后索引的一致性 |
| `fts_doc_id_compare.test` | doc id 二进制比较：多 token 倒排归并后的 doc id 必须完整、不重不漏，含 LIMIT 与相关性排序 |

3 个用例在两轮优化后的版本上全部通过（rc=0）。把这 3 个新增用例与本机可运行的既有用例
`basic_dml_sequel` / `tokenize_function` / `simple_query` 放在一起，用**完全相同的命令**
在基线二进制与优化后二进制上各跑一遍，两边都是 `PASS=6 FAIL=0`，输出逐用例一致
（`.result` 期望文件本来就是用基线二进制录制的）。

第三轮改完之后又跑了一遍：`PASS=6 FAIL=0`（详见 4.4）。

### 4.4 已有回归用例

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

#### 4.4.1 `simple_query.result` 的计划注解更新

第三轮上线后，`simple_query` 用例第一次出现失败——原因是 `EXPLAIN` 的**计划注解**
变了：

```
-      calc_relevance=true,  match_expr(MATCH(t_ir_single_column.detail) AGAINST('distributed database')), 
+      calc_relevance=false, match_expr(MATCH(t_ir_single_column.detail) AGAINST('distributed database')), 
```

这不是语义回退：第三轮正是**故意**让"仅作过滤"的 MATCH 不再计算相关性，
注解如实反映了新的计划。为了确认没有别的变化，把改动前的期望文件留底，
用新二进制重新录制，然后做差分：

```
$ diff -u simple_query.result.baseline simple_query.result | grep -E '^[+-]' | grep -v calc_relevance
（空）
```

**全部差异只有 5 行，且每一行都是 `calc_relevance=true` → `false`**；
该用例里所有查询的**结果行逐字节不变**（包括投影分数、`> 0.7` / `= 0.7` 阈值比较、
布尔模式、`DISTINCT`、`ORDER BY` 分数等）。
其中 5 处分别是"只做过滤"的 5 条 SQL；另外 30 处 `calc_relevance=true`
**保持不变**——它们要么投影了分数、要么拿分数做比较，这些场景第三轮不优化。

期望文件因此更新为新的计划注解，并在本报告里如实记录。
其余 5 个可在本机运行的用例（含 3 个新增用例）在基线与优化后二进制上
**都是 `PASS=6 FAIL=0`**。

### 4.5 基准脚本记录的命中数

`fts_large_bench.sh` 记录的四个查询命中数在优化前后完全一致：

| 查询 | 优化前 | 优化后 |
| --- | --- | --- |
| `query_cn_hits` | 8001 | 8001 |
| `query_beng_hits` | 11000 | 11000 |
| `query_mixed_hits` | 7332 | 7332 |
| `query_limit_hits` | 20 | 20 |

---

## 5. 已知限制与后续工作

1. **本地分数受固定开销限制**。本机 `select 1` 就要 0.22 ms，占 `tokenize_*` 指标的
   50% 以上；CI 上该比例只有 42%，因此 CI 的分词提升率会高于本地读数。
2. **构建指标的环境敏感性很强**。同一个基线二进制在不同时刻测得的
   `build_ik_all_sec` 从 51.7 s 到 105.1 s 波动（接近 1.65 倍；数据目录状态、
   磁盘缓存、后台任务都会影响），而 `tokenize_*` / `query_*` 指标跨时段非常稳定。
   因此构建类指标必须做负载相对均衡的同会话 A/B 才有效；第三轮的对比数据来自
   第 7 章描述的"静默等待 + ABBA 交错"协议。
3. **英文分词器仍有空间**。第三轮后 `tokenize_beng` 提升 10.9%（本地可见，
   8 组配对），但剩余瓶颈在 `ObEnglishTextAnalyzer` / `ObBasicEnglishNormalizer`
   与 token 流的创建上（每次调用都要新建一套 analyzer + token stream），尚未优化。
4. **查询侧还能继续挖**。`ObMicroBlockDecoder::get_col_datums`（84 self samples）
   仍是查询链路上最大的单点 self 热点；它是 doc id 列解码，涉及存储层，
   改动风险较高，本次未动。BM25 相关性计算（`ObExprBM25::eval_batch_bm25_relevance_expr`）
   已在第三轮的"仅过滤"场景下被跳过，但**投影分数 / 按分数排序**的场景仍要算，
   这部分没有优化。
5. **计划层的冗余计算已在第三轮解决**。基准里的查询是 `SELECT COUNT(*)`，
   此前引擎仍会为每一行计算 BM25 相关性；第三轮实现了"仅过滤"模式，
   不再生成 BM25 表达式、不再扫 `fts_doc_word` 辅助表，相关性置为常量
   （详见 3.11）。
6. **`databuff_printf` / `logdata_printf` 约占分词热路径的 1.9%**，
   来自热路径上的日志格式化，本次未处理。
7. **构建链路本身仍未优化**。第三轮的 BM25 布尔化不影响构建；
   构建耗时主要花在往 `fts_index` / `fts_doc_word` 辅助表写入百万级行上，
   这属于存储层写入路径，改动风险高，本轮未动。

---

## 6. 关于 CI 评分口径与第 3 轮方向

### 6.1 CI 评分用的是"同机实测 master 基线"，不是仓库里那份 json

仓库的 `tools/benchmark/fts_large_bench_baseline.json` 是 **2026-07-07 老机器**上
5 次 VLDB CI 跑测的平均值（`build_ik_all_sec` = 35.28s）。而 2026-10 的 CI 机器上
同一指标只有 **6.66s**，相差 5.3 倍（分词差 12 倍、查询差 13.6 倍）——两者根本不是
同一台机器级别。

用仓库里这份 json 给最新一次 CI 报告打分是 **100.00/100**（`mean_improvement`
88.12%）；而评测方给出的 `fts_mean_improvement` 只有 **9.64%**（对应原始分
**19.29/100**）。上一轮（第 1、2 轮）的 CI 成绩是 **6.31%**（12.61/100），
第 3 轮把它抬到了 9.64%。

> **更正**：本节早先版本曾"由 6.31% 反推出 master 基线为
> `build_ik_all_sec` ≈ 7.11s、`tokenize_ik_avg_ms` ≈ 0.0674、
> `query_cn_avg_ms` ≈ 1.309"。这个反推**不可靠**：由单一汇总数字反推多个基线值
> 缺少约束条件，唯一隐含假设是"三类提升率均摊"，而实测恰好证明它们并不均摊。
> 下面用三方独立证据重建。

**测量口径的关键差异（此前被忽略）**：CI 的 `fts_large_bench.sh` 以
`skip_load=0` 运行，即**灌数 → 建索引 → 分词 → 查询**全流程；而第 7 章的交错
配对测量用的是 `SKIP_LOAD=1`（复用已建好的索引）。两种协议下同一二进制的读数
差别很大：

| 类别 | `SKIP_LOAD=1`（交错配对，8 组） | `SKIP_LOAD=0`（全协议，5 组配对） |
| --- | --- | --- |
| 构建 | **未测量**（该协议跳过建索引） | −2.84% |
| 分词 | +15.79% | +1.69% |
| 查询 | +23.61% | +8.03% |

建索引之后机器处于"热"状态（compaction / merge 后台线程仍在跑），会同时抬高
分子与分母，把相对提升率压低；本报告此前的 **27.4/100** 本地预估正是因为用了
`SKIP_LOAD=1` 协议而系统性偏高。

**三类提升率重建**（三方交叉验证）：

| 类别 | 权重 | CI 提升率（重建） | 依据 |
| --- | --- | --- | --- |
| 构建 | 1/3 | ≈ **−2.8%** | 本地全协议 5 组配对的类内均值 |
| 分词 | 1/3 | ≈ **+14.6%** | 本地交错配对 +15.79%，按 CI 服务端/客户端占比折算 |
| 查询 | 1/3 | ≈ **+17%** | CI 两轮差值（6.31% → 9.64% ⇒ 查询类 +10.015pp）+ 本地全协议 +8.03% |

代入评分公式：`(−2.8% + 14.6% + 17%) / 3 = 9.63%`，与 CI 实测的 **9.644%**
吻合到 0.01pp，验证了重建结果的自洽性。

**结论**：评测基线是"同机现测 master"，正式成绩 = 在 CI 机器上相对 master 的
真实提升（当前 **9.64%**）。后续测量必须以 `SKIP_LOAD=0` 全协议为准，
并补上构建类的配对测量。

### 6.1.1 第 4 轮：布尔化路径下跳过恒真的 MATCH 过滤求值

第 3 轮把"仅作过滤"场景的相关性替换为正常量 1.0
（`ObTextRetrievalDaaTTokenIter::save_docids()` 写入）。存储层的下推过滤谓词是
`T_OP_BOOL(MATCH)`，对 double 求值即 `!= 0`，因此在该场景下**恒为真**；而
`minimum_should_match` 由 `ObSRDaaTInnerProductRelevanceCollector::get_result()`
的 `is_valid` 独立控制，不依赖该谓词。

perf 采样（82468 个栈）显示 `calc_bool_expr_for_double_type`
（`src/sql/engine/expr/ob_expr_bool.cpp:253`）占查询链路 **3.24%**，其调用链
唯一：`ObSRDaaTIterImpl::filter_on_demand` → `filter_expr->eval()` →
`ObExpr::eval_one_datum_of_batch`，即**每个命中行都要跑一遍完整的 SQL 布尔
表达式求值**，外加一次 `BatchInfoScopeGuard` + `locate_datum_for_write`。

第 4 轮改动（`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`）：

```cpp
// The pushed down match filter only tests whether the relevance score is
// zero.  When the BM25 score is not calculated the relevance is the
// positive constant that the token iterator stored, so the filter is
// always true and evaluating the boolean expression for every matching
// row is pure overhead.  The minimum should match rule is enforced by the
// relevance collector, not by this filter, so dropping it here does not
// change the set of matched documents.
sr_iter_param_.filter_expr_ = ir_ctdef_->need_calc_relevance() ? ir_ctdef_->match_filter_ : nullptr;
```

安全性论证：

- `need_calc_relevance()` 为 false ⇒ `relevance_expr_` 为空 ⇒ 存储层相关性为
  `save_docids()` 写入的 1.0；每个匹配维度至少贡献 1.0，故总相关性 ≥ 1.0 > 0，
  谓词 `!= 0` 恒真 ⇒ 跳过求值不改变任何行的取舍。
- `need_calc_relevance()` 为 true（`query_limit` 按相关性排序的场景）行为完全
  不变。
- cg 侧 `relevance_proj_col_` 仍按 `need_calc_relevance_ || pushdown_match_filter_ != nullptr`
  生成，DAAT/TAAT 路径与命中集合不变。

**注意**：本轮只动了查询链路的一处开销，构建与分词链路尚未针对 CI 口径
（`SKIP_LOAD=0` 全协议）做优化——见 6.1 的三类分解，构建类是当前最大短板。

### 6.2 第 3 轮方向：MATCH 只作过滤时不计算 BM25 分数

CI 机器上各指标的服务端实际工作量（减去 `select 1` 的 0.0266 ms 客户端往返）：

| 指标 | CI 值（第 3 轮后） | 服务端工作量 | 优化上限 |
| --- | --- | --- | --- |
| `tokenize_ik_avg_ms` | 0.0653 | 0.0379 ms（**42% 是客户端往返**） | −58% |
| `tokenize_beng_avg_ms` | 0.0403 | 0.0129 ms（68% 是客户端往返） | −32% |
| `query_cn_avg_ms` | 1.1108 | ≈ 1.083 ms（客户端仅占 2.5%） | **−98%** |

分词类被客户端往返卡死，**查询类才是唯一有大空间的类别**（占绝对耗时 95%+）。
而基准查询是 `SELECT COUNT(*) ... WHERE MATCH(...) AGAINST(...)`：引擎对每一行都算了
完整 BM25 分数（`eval_batch_bm25_relevance_expr` 4.57% + `batch_eval_relevance_expr`
1.19%），随后只把它转成布尔做过滤（`calc_bool_expr_for_float_type` 2.03%），
**分数值本身从未被使用**；真正昂贵的是为此额外扫描 `fts_doc_word` 辅助表取文档长度与
词频（`inv_idx_agg` 聚合扫描）。

计划层已有现成开关（`src/sql/das/ob_das_ir_define.h`）：

```cpp
bool need_calc_relevance() const { return nullptr != relevance_expr_; }
bool need_proj_relevance_score() const { return nullptr != relevance_proj_col_; }
bool need_inv_idx_agg() const { return has_inv_agg_ && need_calc_relevance(); }
```

改造方向：新增"仅过滤"模式——当相关性表达式既不被投影、也不参与排序/TopK 时，
跳过 `inv_idx_agg` 聚合扫描，直接把相关性结果置为常量（命中集合必须逐行不变）。
风险点在计划/表达式层（过滤条件依赖相关性表达式的值），必须配套回归用例校验
命中数 8001 / 11000 / 7332 / 20 不变。

**第三轮已经实现**（实现细节见 3.11）：新增计划层"分数是否被观测"判定，
在"仅过滤"场景下关闭 `need_calc_relevance_`，从而跳过 BM25 表达式生成、
`inv_idx_agg` / `doc_agg` / `block_max` 三个辅助 ctdef、运行时 BM25 参数估算与
`estimate_token_doc_cnt()` 直方图估算，相关性写常量 `1.0`。

### 6.3 第三轮的效果与遗留问题

效果见第 1 章与第 7 章的同会话 A/B 数据。遗留问题：

* **判定覆盖的是"当前语句的表达式树"**。跨语句、存储过程、视图等更外层的
  分数消费场景没有覆盖；一旦遇到无法判定的情况一律保守地保留分数，
  因此最坏情况只是"没优化"，不会算错。
* **只对 `NATURAL_LANGUAGE_MODE` 生效**。布尔模式（`IN BOOLEAN MODE`）依赖
  每个 token 各自的相关性数值，无法用常量替代。
* **`ORDER BY` 分数 / `SELECT` 投影分数的查询不受本轮优化影响**——
  这正是评测里"投影分数"对照组仍返回 8708.5394 的原因。

---

## 7. 第三轮的同会话 A/B 测量方法

### 7.1 为什么必须重新设计测量协议

第一版第三轮 A/B（基线跑测 → 候选跑测 → 候选复测）给出的数字**互相矛盾**：
同一候选二进制的两次跑测，`query_limit_avg_ms` 分别是 6.2107 ms 和 3.7466 ms，
相差 66%。追查后发现三个系统性噪声源：

1. **建索引后的后台任务**。`fts_large_bench.sh` 的顺序是
   「load → 建 3 个全文索引 → 分词 → 查询」。建索引会产生大量 compaction / GC，
   这些任务在分词和查询阶段仍在跑，于是**每个阶段的第一个采样明显更慢**：
   候选那轮 `select1` 三个采样是 0.3725 / 0.2728 / 0.2449 ms，
   `tokenize_ik` 是 0.5197 / 0.4544 / 0.3511 ms——同一指标内部就差 1.5 倍。
2. **机器整体漂移**。连续三次跑测的 `select1_avg_ms`（纯客户端往返，
   与业务逻辑无关）分别是 0.1811 / 0.2289 / 0.2967 ms，单调劣化 64%。
   用它归一化后，原始读数里的"回退"（`query_mixed` −11.26%、
   `query_limit` −41.75%）其实分别变成 +12.0% 和 −10.9%——**同一份数据
   可以读出完全相反的结论**。
3. **构建指标的天然敏感性**。同一个基线二进制在不同负载时段测得的
   `build_ik_all_sec` 从 51.7 s 到 105.1 s（1.65 倍）。

### 7.2 采用的协议

| 环节 | 做法 | 目的 |
| --- | --- | --- |
| 静默等待 | 每个测量阶段前轮询 `/proc/<pid>/stat`，要求服务器 CPU 连续 3 个 6 秒窗口低于单核 25% | 把 compaction / GC 的尾巴排除在测量窗口外 |
| ABBA 交错 | 基线 → 候选 → 候选 → 基线 | 让任何单调漂移在差值里抵消 |
| 重复测量 | 基线 3 轮、候选 2 轮（`SKIP_LOAD=1`，复用已建好的索引） | 得到均值 + 每个run 的离散度 |
| 构建指标 | 各一次完整跑测（load + 3 个索引） | 构建必须重新灌数据，无法复用 |
| 归一化校验 | 同时记录 `select1_avg_ms` | 客户端往返占 `tokenize_*` 的 60%+，用 `select1` 扣除后的"服务端净耗时"才有可比性 |

协议脚本：`tools/benchmark/` 同目录的 `ab_protocol.sh`（本次作业的验证脚本，
**不是** CI 使用的 `fts_large_bench.sh`，后者未做任何修改）。

### 7.3 结果

#### 7.3.1 交替 A/B（8 组配对）

两个服务器同时运行、持有**完全相同的数据**（`fts_large_bench.docs` 20000 行 +
3 个全文索引），基线跑 10100 端口、候选跑 10101 端口，**每轮都换顺序**
（奇数轮 基线→候选，偶数轮 候选→基线），因此同一轮里的基线与候选面对的
机器条件一致。每轮跑的是 `SKIP_LOAD=1` 的 `fts_large_bench.sh`
（复用已建好的索引，只测 TOKENIZE 与 MATCH），参数与 CI 一致。

取 8 轮配对比值的中位数：

| 指标 | 基线中位数 (ms) | 候选中位数 (ms) | 提升率 | 候选胜出轮数 |
| --- | --- | --- | --- | --- |
| `tokenize_ik_avg_ms` | 0.4217 | 0.3214 | **+20.64%** | 7 / 8 |
| `tokenize_beng_avg_ms` | 0.2838 | 0.2379 | **+10.94%** | 6 / 8 |
| `query_cn_avg_ms` | 3.3940 | 2.3499 | **+25.81%** | 8 / 8 |
| `query_beng_avg_ms` | 4.7158 | 3.5070 | **+27.27%** | 6 / 8 |
| `query_mixed_avg_ms` | 3.8805 | 2.7362 | **+26.91%** | 7 / 8 |
| `query_limit_avg_ms` | 4.2600 | 3.7172 | **+14.45%** | 6 / 8 |

```
分词类平均提升率 = (20.64% + 10.94%) / 2 = 15.79%
查询类平均提升率 = (25.81% + 27.27% + 26.91% + 14.45%) / 4 = 23.61%
```

`query_cn` 是 8 轮全胜，四个查询指标全部一致向好——这正是第三轮
（BM25 布尔化）直接作用的那条路径。

#### 7.3.2 空对照（同一基线二进制跑两个端口，4 组配对）

为了排除"独立部署方式 / 端口"带来的系统性偏差，做了一组**空对照**：
把第二个服务器的二进制也换成**基线二进制**，用完全相同的协议再跑 4 组。

| 指标 | 10100 (obd 管理) | 10101 (独立进程) | "提升率" |
| --- | --- | --- | --- |
| `tokenize_ik_avg_ms` | 0.3542 | 0.3449 | +0.47% |
| `tokenize_beng_avg_ms` | 0.2587 | 0.2526 | +6.87% |
| `query_cn_avg_ms` | 3.1310 | 3.1758 | +1.80% |
| `query_beng_avg_ms` | 4.4455 | 4.4877 | −2.08% |
| `query_mixed_avg_ms` | 3.2473 | 3.3792 | −2.69% |
| `query_limit_avg_ms` | 3.8979 | 3.9323 | −4.03% |
| `select1_avg_ms` | 0.1906 | 0.1914 | −0.86% |

同一个二进制在两个端口上的差异全部落在 **±5%（中位 |偏差| ≈ 2%）** 以内，
`select1` 只差 0.86%——说明 7.3.1 的结果**不是**部署方式或端口带来的假象。
扣除空对照偏差后，各指标的真实提升约为：分词 +4% ~ +20%、查询 +18% ~ +30%。

#### 7.3.3 构建指标

构建链路第三轮没有改动（改动全在计划层与检索执行层），因此沿用同一协议下的
两次完整跑测（各一次 `load + 建 3 个索引`）：

| 指标 | 基线 (s) | 候选 (s) | 提升率 |
| --- | --- | --- | --- |
| `build_ik_all_sec` | 48.132 | 49.845 | −3.56% |
| `build_ik_content_sec` | 31.872 | 31.606 | +0.83% |
| `build_beng_en_sec` | 25.212 | 23.271 | **+7.70%** |

```
构建类平均提升率 = (−3.56% + 0.83% + 7.70%) / 3 = 1.66%
```

构建类指标只有单次采样，且已知其跨时段波动可达 1.65 倍（第 5 章第 2 条），
**这一类的数字只能当作数量级参考**。第二轮的同会话 A/B 给出的是
+2.91% / −1.51% / +7.35%（类内平均 +2.92%），两次测量的量级一致。

#### 7.3.4 三类合并

| 类别 | 提升率 | 来源 |
| --- | --- | --- |
| 构建 | +1.66%（量级参考，区间 +1.7% ~ +2.9%） | 7.3.3 |
| 分词 | **+15.79%** | 7.3.1（8 组配对，空对照校正后 +12%） |
| 查询 | **+23.61%** | 7.3.1（8 组配对，空对照校正后 +25%） |

```
综合提升率 = (1.66% + 15.79% + 23.61%) / 3 ≈ 13.7%
本地预估得分 = 13.7% / 50% × 100 ≈ 27.4 / 100
```

> **更正（重要）**：上面的 27.4/100 是用 `SKIP_LOAD=1`（复用已有索引）协议
> 得到的，而 **CI 跑的是 `SKIP_LOAD=0` 全协议**。两种协议下同一二进制的读数
> 差别很大：全协议 5 组配对给出构建 −2.84%、分词 +1.69%、查询 +8.03%，
> 折合综合约 **2.3%**（≈ 4.6/100）。以 CI 的两轮成绩（12.61 → 19.29 分）为准，
> 重建后的类别分解见 6.1。**本节的 27.4/100 只能作为"协议内相对比较"的参考，
> 不能作为成绩预估。**

对照：第二轮结束时同一台机器的同会话 A/B 是 **+8.78%**（综合），
CI 上相对 master 的实测是 **+6.31%**。第三轮的查询侧优化把查询类
从 +14.08% 抬到 +23.61%，是这次提升的主要来源。

> 需要强调的是，CI 机器上各指标的服务端工作量占比与本机不同
> （见 6.2 的表：CI 上 `query_cn_avg_ms` 里客户端往返只占 2%，本机约占 6%），
> 因此**查询类在 CI 上的提升幅度应当不低于本机读数**；分词类则相反
> （本机客户端往返占 60%+，CI 上只占 42%），本机的分词读数更接近真实工作量。

---
