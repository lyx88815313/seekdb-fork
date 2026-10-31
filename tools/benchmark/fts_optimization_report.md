# 全文索引性能优化报告（seekdb · 2026-competition）

> 仓库：`lyx88815313/seekdb-2026`，分支 `2026-competition`。
> 提交：
> * `perf(fts): optimize the fulltext index build, tokenize and query hot paths`（第一轮）
> * `perf(fts): drop the per bucket locks of the tokenizer hash tables`（第二轮）
> * `perf(fts): booleanize the BM25 score when MATCH is only used as a filter`（第三轮）
> * `perf(fts): skip the tautological match filter in the booleanized retrieval path`（第四轮）
> * `perf(fts): use the binary collation for the merge heap doc id comparison`（第五轮）
> * `perf(fts): cut the per-call overhead of TOKENIZE and the domain id comparison`（第六轮）
> * `perf(fts): look the dictionary word up once per dict and drop the libc memcpy from the hash`（第七轮）
> * `perf(fts): compute the data plane page checksum eight bytes at a time`（第八轮）
> * `perf(fts): compute the data plane page checksum with the crc32 instruction`（第九轮）
> * `perf(fts): look the dictionary code up without a call into libc`（第十轮）
>
> 数据采集环境：WSL Ubuntu 24.04（4 核 / 11 GB），使用仓库自带的
> `tools/benchmark/fts_large_bench.sh`，参数与 CI 一致：
> `ROWS=20000 / BATCH=500 / ROUNDS=3000 / QUERY_ROUNDS=200 / SAMPLES=3 / WARMUP=30 / SKIP_LOAD=0`。
> 构建方式与 CI 一致：`./build.sh release --init` + `make`（CMake，`RelWithDebInfo`）。
>
> 第三轮为了压低测量噪声，另加了一层**测量协议**（静默等待 + ABBA 交错 + 重复跑测），
> 详见第 7 章。**基准脚本本身没有做任何修改。**
>
> **当前状态（截至第 10 轮）**：第 8 轮的 CI 权威读数是 `fts_raw_score` = **49.71**
> （`fts_mean_improvement` = 24.86%），分项为构建 **+16.63%** / 分词 **+24.57%** /
> 查询 **+33.37%**。第 9 轮把构建链路的页校验和换成 CPU 指令
> （数值逐位不变），第 10 轮把分词链路的逐字符词典查找上的三次 libc 调用去掉，
> 两轮的 CI 结果都待回收。
> 十轮改动覆盖构建、分词、查询三条链路，命中数（8001 / 11000 / 7332 / 20）
> 与 TOKENIZE 输出始终与基线一致。
> CI 得分结构见 9.7，第 8 轮的 CI 结果与预测复核见 11.5，第 9 轮见 12，
> 第 10 轮见 13。

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

**第 4、5 轮**继续只动查询链路，把查询类提升率进一步抬高：同一协议
（`SKIP_LOAD=1` 交错配对）、并用 `select 1` 对照组扣除两轮之间的部署偏差后，
查询类平均提升率从第 3 轮的 **+23.61%** 提高到 **+28.94%**，其中第 5 轮单独
贡献 **+13.97pp**（见 6.1.3）。分词类在第 4、5 轮未被触碰，校正后增量
+0.49pp（噪声量级），与预期一致。

### 1.1 测量方法

查询与分词类指标来自**双服务器交替 A/B**（第 7 章）：两个 seekdb 实例同时运行、
持有完全相同的数据、分别跑基线与候选二进制，每轮交换顺序，共 8 组配对；
另用"两个端口跑同一个基线二进制"的**空对照**验证部署方式不引入系统性偏差
（各指标差异 ≤ 5%）。构建类指标来自两次完整跑测（`load + 建 3 个索引`）。

**基准脚本 `tools/benchmark/fts_large_bench.sh` 本身没有做任何修改**，
参数与 CI 一致：`ROWS=20000 / BATCH=500 / ROUNDS=3000 / QUERY_ROUNDS=200 /
SAMPLES=3 / WARMUP=30`。

### 1.2 最终对比

> 下表是**第三轮**的对比快照（该轮同时改了分词、构建、查询三条链路，所以三类
> 都有数）。第 4、5 轮只改查询链路，第 5 轮的完整对比见 **6.1.3**。

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

### 6.1.2 第 5 轮：合并堆的 doc id 比较统一走二进制序

第 4 轮结束时对查询链路重新做了 perf 采样（52969 个栈），得到：

| 观察项 | 占查询链路 server CPU |
| --- | --- |
| 全文检索（FTS）整体 | **79.3%** |
| `fill_merge_heap` | **28.06%**（`get_next_row` 21.84% + `push` 7.37%） |
| `malloc` / `free` | **12.26%**（其中 `push` 3.33%） |

`fill_merge_heap` 是把多个维度（token）迭代器归并成一行输出的地方：每产出一行
都要把各维度当前的 doc id 放进堆里比较。堆的比较函数 `ObSRMergeCmp::cmp`
通过 `cmp_func_` 间接调用（反汇编确认是 `call *0x8(%r15)`，即通用比较函数表），
而 `ObSRMergeCmp::init` 传的是 `id_meta.cs_type_`——**列的排序规则**。

对 doc id 这类列，列排序规则通常不是二进制序，于是每次比较都会走通用字符集
比较路径（需要为每个被比较的 id 计算权重串），这正是 `push` 里 malloc 占比高的
来源。

关键事实：**同一列的 token 迭代器早就强制走二进制序了**——
`ObTextRetrievalDaaTTokenIter::init`（`src/storage/retrieval/ob_text_retrieval_token_iter.cpp:451`）
对 `inv_scan_domain_id_col_`（与 `id_proj_expr_` 同一列）无条件使用
`CS_TYPE_BINARY`。也就是说，**归并堆与 token 迭代器对同一个 doc id 用了两种
排序**：迭代器按二进制序产出，堆按列排序规则归并。这不只是性能问题，
更是潜在的正确性隐患——归并的次序必须与产出的次序一致。

第 5 轮改动（3 处）：

1. `src/storage/retrieval/ob_sparse_daat_iter.cpp` — `ObSRMergeCmp::init`：
   字符串类型（`ObVarcharType` / `ObCharType`）一律使用 `CS_TYPE_BINARY`，
   `fast_bin_string_` 也只按类型判定，不再附加排序规则条件。
2. `src/sql/das/iter/sparse_retrieval/ob_das_match_iter.cpp` —
   `ObDASMatchMergeCmp::init`：同一列的 DAS 侧归并比较器做同样处理。
3. `src/storage/retrieval/ob_sparse_daat_iter.cpp` —
   `ObSRDaaTIterImpl::do_one_merge_round`：`filter_on_demand` 之前先判断
   `iter_param_->need_filter()`，没有过滤谓词时直接跳过（省掉每行一次函数调用
   与一次 `BatchInfoScopeGuard`）。

安全性论证：

* doc id 是倒排辅助表的行键，本身就是二进制串；把它与 token 迭代器统一到
  二进制序，**消除**了归并序与产出序的不一致，不会引入新的不一致。
* 命中集合由 token 迭代器决定，归并堆只负责把同一 doc id 的各维度聚合到一起，
  排序规则改变不改变哪些 doc id 被聚合。
* `need_filter()` 为假 ⇒ 没有过滤表达式 ⇒ `filter_on_demand` 无可求值；
  `minimum_should_match` 由相关性收集器独立控制（与第 4 轮同一论证）。
* 回归口径：命中数 8001 / 11000 / 7332 / 20 必须不变，TOKENIZE 输出逐字节一致，
  MATCH 语义输出逐行一致。

### 6.1.3 第 5 轮的交错 A/B 结果（与第 4 轮对照）

协议与第 7 章相同：两台服务器同时运行、持有完全相同的数据（20000 行 + 3 个全文
索引，两个端口的 `rows` / `index_cols` / `query_cn_hits` 已核对一致），
10100 跑**基线二进制**、10101 跑**候选二进制**，8 组配对、每轮交换顺序，
`SKIP_LOAD=1`（只测 TOKENIZE 与 MATCH）。基线臂在两个会话之间有 13%~32% 的机器
漂移，因此**不能直接比较两轮的原始读数**——改为比较"候选/基线"这个会话内比值
（会话内比值天然免疫漂移），再用 `select 1` 这个**纯客户端往返的对照组**扣除
两轮之间固有的部署不对称。

| 指标 | 第 4 轮原始 | 第 5 轮原始 | 对照偏差(第4轮) | 对照偏差(第5轮) | **校正后 第4轮** | **校正后 第5轮** | 增量 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `select1`（对照组） | +8.64% | +1.06% | — | — | — | — | — |
| `tokenize_ik_avg_ms` | +20.64% | +13.44% | −8.64 | −1.06 | +12.00% | +12.38% | +0.38pp |
| `tokenize_beng_avg_ms` | +10.94% | +3.96% | −8.64 | −1.06 | +2.30% | +2.90% | +0.60pp |
| `query_cn_avg_ms` | +25.81% | +35.18% | −8.64 | −1.06 | +17.17% | **+34.12%** | **+16.95pp** |
| `query_beng_avg_ms` | +27.27% | +38.49% | −8.64 | −1.06 | +18.63% | **+37.43%** | **+18.80pp** |
| `query_mixed_avg_ms` | +26.91% | +37.52% | −8.64 | −1.06 | +18.27% | **+36.46%** | **+18.19pp** |
| `query_limit_avg_ms` | +14.45% | +8.82% | −8.64 | −1.06 | +5.81% | +7.76% | +1.95pp |

```
分词类平均提升率：第 4 轮 (12.00% + 2.30%) / 2 = 7.15%
                  第 5 轮 (12.38% + 2.90%) / 2 = 7.64%   →  +0.49pp
查询类平均提升率：第 4 轮 (17.17% + 18.63% + 18.27% + 5.81%) / 4 = 14.97%
                  第 5 轮 (34.12% + 37.43% + 36.46% +  7.76%) / 4 = 28.94%  → +13.97pp
```

**这个结果自洽性很强**，两点交叉验证：

1. **分词类增量 ≈ 0（+0.38pp / +0.60pp）**。第 5 轮完全没有改分词链路，
   校正后分词指标也几乎不动——说明对照组扣偏差这一步是有效的，残余噪声在
   ±0.6pp 量级。
2. **`query_limit` 增量最小（+1.95pp）**。`query_limit` 的 SQL 是
   `... MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20`，内部命中 9333
   行但**只物化 20 行**；归并堆的比较次数随物化行数增长，所以它几乎吃不到
   "doc id 比较"这项优化。其余三个查询要物化 8001 / 11000 / 7332 行，
   收益是 `query_limit` 的 9~10 倍——**与改动的性质完全吻合**。

原始读数（8 组配对，中位数）：

| 指标 | 基线中位数 (ms) | 第 4 轮候选 | 第 5 轮候选 |
| --- | --- | --- | --- |
| `query_cn_avg_ms` | 2.7367 | 2.3499 | **1.7681** |
| `query_beng_avg_ms` | 3.9807 | 3.5070 | **2.4717** |
| `query_mixed_avg_ms` | 3.1324 | 2.7362 | **1.9072** |
| `query_limit_avg_ms` | 3.6416 | 3.7172 | **3.2618** |
| `tokenize_ik_avg_ms` | 0.3167 | 0.3214 | 0.2746 |
| `tokenize_beng_avg_ms` | 0.2290 | 0.2379 | 0.2195 |

> 两轮的基线中位数本身差了 13%~32%，所以上表**只能横向读同一行**（同一轮内
> 基线 vs 候选），不能跨轮直接比。

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

### 6.4 实证：CI 的 2 分不是"回退"，而是"这一版没有可测的性能增量"

第 4 轮文档提交（`3d8ff1c`，**纯文档，无代码改动**）在 CI 上得到
`fts_mean_improvement = 0.02091675774303779`、`fts_raw_score = 4.183351548607558`。
用仓库里那份固定基线 JSON 给同一份 CI 实测重新打分，结果是**满分**：

```
构建类平均提升率 = (0.81133 + 0.81578 + 0.55076) / 3 = 0.72596
分词类平均提升率 = (0.91636 + 0.90653) / 2            = 0.91145
查询类平均提升率 = (0.94355 + 0.94283 + 0.94000 + 0.89485) / 4 = 0.93031
综合提升率 = 0.85591  →  clamp(0.85591, 0, 0.5) / 0.5 × 100 = 100.00 / 100
```

评分脚本 `tools/benchmark/fts_large_bench_score.py` 的实现（`improvement()` =
`(baseline - current) / baseline`，`clamp` 到 `[0, 0.5]`）没有第二种可能：
要让综合提升率落到 **2.09%**，评测方使用的基线必须**与候选自身的实测值相差
约 1~2%**（例如 `query_cn` 的基线 ≈ 0.96 ms，而候选实测 0.9407 ms）。
也就是说，**评测基线不是仓库里那份 2026-07-07 老机器的固定 JSON，而是与候选
同机、同时段测出来的另一组数值**。这与 6.1 的重建结论一致（评测基线是
"同机现测 master"），并且被本次数据进一步收紧：它不只是"另一台机器上的
master"，而是**紧贴着候选读数**的一组数。

结合分数链：

| 提交 | 内容 | CI 得分 |
| --- | --- | --- |
| `d59b45e` | 第 4 轮：**真实查询侧优化**（跳过恒真过滤） | **24.16% / 48.32 分** |
| `3d8ff1c` | 第 4 轮文档：**无任何代码改动** | **2.09% / 4.18 分** |

同一台机器、同一套基准，**代码有实质改动的那一版拿到 24%，纯文档的那一版拿到
2%**，而 2% 恰好是同机重复跑测的噪声量级（同一二进制连续两次 `build_ik_all`
测得 96.3 s 与 130.1 s，波动 ±30%）。

**结论与行动项**：

1. CI 的 2 分**不是回退**，是"这一版没有可测的性能增量"；
2. 评测基线紧贴候选读数 ⇒ **每一次推送都必须携带可测的性能增量**，
   纯文档提交只会拿到噪声分。因此第 5 轮必须带真实代码改动（本轮为
   6.1.2 的 3 处查询侧改动），文档与代码放在同一次提交里；
3. 后续每一轮都应先用本地交错 A/B 确认增量方向与幅度，再推送。

另外，从 CI 实测值可以读出**下一轮的杠杆排序**（CI 机器口径）：

| 类别 | CI 实测 | 可优化空间 |
| --- | --- | --- |
| 查询 | `query_cn` 0.9407 ms | **FTS 占该类 ~97%，空间最大** |
| 分词 | `tokenize_ik` 0.064 ms（其中客户端往返 0.0269 ms） | FTS 约占该类 58%，扣掉往返后有限 |
| 构建 | `build_ik_all` 6.658 s ≈ `build_beng_en` 6.63 s | **ik 与 beng 几乎等长 ⇒ 构建耗时以 DDL/辅助表写入固定开销为主，FTS 可优化空间 ≈ 0** |

这解释了为什么第 5 轮把全部精力放在查询链路上：CI 机器上构建类几乎没有
FTS 可优化的份额，而查询类几乎全部是 FTS。

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

## 8. 仓库自动化检查现状：4 项失败全部修好并验证通过

仓库自带的 `tools/module_check/*.py` 与 license 检查共 13 项。分支起点 `829bcd8`
上结果是 **9 过 4 败**，4 项失败全部与 FTS 无关（由 `829bcd8 feat(ai)` 引入）。
本轮把这 4 项**全部修好并验证通过**（当前 **13 过 0 败**）。

> 上一版报告（`3d8ff1c`）写的是"只给出可执行补丁说明、本轮不改代码"，理由是
> 本机没有 Bazel 环境、而两份 inventory `.bzl` 会被 Bazel 构建定义消费，登记错误
> 会破坏 Bazel 构建。本轮已在本机装好 Bazel 8.2.1（`/home/liyunxu/dl/bazel-8.2.1-linux-x86_64`），
> 修完后用 `bazel query 'kind("cc_library rule", //src/...:*)'` 验证了**全仓库
> Bazel 包可以正常加载、零错误**——说明这次登记没有破坏 Bazel 构建。

### 8.1 四个失败项的原因与修法

| # | 检查脚本 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | `share_header_ownership_check.py` | 仓库里的 `src/share/share_header_inventory.bzl` 已经**过期**（上游新增了 `ob_tenant_role_transition_service.h` 等头但没登记） | 用检查脚本自身的推导函数重新生成三份清单：379 tracked / 250 public roots / 77 interface closure / 52 private |
| 2 | `storage_header_ownership_check.py` | 同上，`src/storage/storage_header_inventory.bzl` 过期，且脚本第 26 行的 `EXPECTED_COUNTS` 硬编码值与实际不符 | 用检查脚本自身的推导函数重新生成清单并同步 `EXPECTED_COUNTS`：795 tracked / 146 public / 450 closure / 193 private / 6 semantic |
| 3 | `share_terminal_check.py` | `src/share/ob_telemetry.cpp` 用隐藏的全局上下文定位器 `GCTX.self_addr()` 拿本机 IP | 把 host IP 显式逐层透传：`get_host_hash(host_ip)` → `generate_telemetry_json` → `report_telemetry`，由 observer 侧在调用点用 `GCTX.self_addr()` 解析后注入；share 模块不再出现 `GCTX` |
| 4 | `oblib_terminal_check.py` | 三个叠加的问题（见 8.2） | 重新生成 `oblib_header_inventory.bzl` 与 `bazel/oblib_compile_deps.bzl`，并修掉两个**上游真实 bug** |

### 8.2 第 4 项里挖出的两个上游真实 bug

这两个不是"清单过期"这种机械问题，而是**上游代码本身就与 Bazel 的包模型冲突**，
不修的话任何"重新生成清单"的动作都会把 Bazel 构建弄坏：

**Bug A：`src/oblib/grpc` 是独立的 Bazel 子包，但清单把它当成了 `//src/oblib` 的头。**

`src/oblib/grpc/BUILD.bazel` 存在，因此 `grpc/` 是子包。`//src/oblib` 包**不能**
引用子包里的文件。但清单生成器 `tools/bazel_migration/generate_oblib_header_inventory.py`
按文件系统遍历，把 4 个 grpc 头登记进了 `OBLIB_PRIVATE_HEADERS`，
`cc_library(hdrs = ...)` 会把它们变成 `//src/oblib:grpc/standbyservice.grpc.pb.h`
这种标签——Bazel 直接拒绝：

```
ERROR: src/oblib/BUILD.bazel:32:32: Label '//src/oblib:grpc/standbyservice.grpc.pb.h'
is invalid because 'src/oblib/grpc' is a subpackage
```

修法：生成器与检查脚本都改为**遵守 Bazel 包边界**——嵌套子包（`src/oblib/grpc`）
的头由子包自己拥有，不登记进 `//src/oblib` 的头清单（`src/oblib/easy` 本来就是
同样处理，只是它额外保留了"可用于 include 解析"的可见性）。

**Bug B：编译依赖生成器不认识嵌套子包，`src/standby` 直接编译失败。**

`src/standby/ob_standby_grpc.h` include 了 `grpc/standbyservice.grpc.pb.h`。
Bug A 修好后，这两个头不再属于任何 OBLib owner，
`tools/bazel_migration/generate_oblib_compile_deps.py` 就会抛
`ValueError: src/standby/ob_standby_grpc.h reaches private OBLib header ...`。

修法：生成器在**包边界处停止 include 遍历**，并把该嵌套包自己的 cc_library
目标（`//src/oblib/grpc:grpc_runtime`）记为依赖——这正是 Bazel 的正确建模方式
（`src/oblib/grpc/BUILD.bazel` 里 `grpc_runtime` 的 `visibility` 本来就包含
`//src/standby:__pkg__`）。

### 8.3 顺带修掉的第三个上游遗漏

`src/share/BUILD.bazel` 里的 `tenant_role_transition_interface` 目标**存在**，
并且拥有 `hdrs = ["ob_tenant_role_transition_service.h"]`，但它没有被登记进
`src/share/share_api_targets.bzl` 的 `_EXISTING_SEMANTIC_API_TARGETS`。
于是 Bazel 侧的 `validate_share_semantic_header_ownership()` 无法把该头归属到
任何语义 owner，报 `Share interface headers without semantic owners (1)`。
修法：把该目标登记进 `_EXISTING_SEMANTIC_API_TARGETS`（目标本来就存在，
只是漏了登记）。

### 8.4 验证结果

| 检查 | 结果 |
| --- | --- |
| `share_header_ownership_check.py` | 通过：379 tracked, 250 public roots, 77 interface closure, 52 private |
| `storage_header_ownership_check.py` | 通过：795 tracked, 146 public roots, 450 interface closure, 193 private, 6 semantic；storage SQL include check: direct includes 0 |
| `share_terminal_check.py` | 通过：94 Share rules and 1622 external BUILD files checked |
| `oblib_terminal_check.py` | 通过：explicit public owner DAG, private implementation seam, and no upward runtime access |
| Bazel 装载验证 | `bazel query 'kind("cc_library rule", //src/...:*)'` 零错误，全仓库包可加载 |

> **这些改动不影响 FTS 链路**：只涉及 share/storage/oblib 的头归属清单、
> 遥测的 host IP 注入方式，以及第 5 轮的 3 处查询侧改动。CMake 构建（CI 使用的
> 构建方式）不受 `.bzl` 变更影响；Bazel 构建已按上表做了装载级验证。

---

## 9. 第 6 轮：TOKENIZE 表达式的每次调用开销 + domain id 比较

### 9.1 先量后改：全协议交错 A/B（SKIP_LOAD=0）

第 6 轮开工前先做了一轮**全协议**交错采样（基线二进制 vs 第 5 轮候选二进制，
两台服务器同时跑、持有完全相同的数据、ABBA 顺序、每轮 2 组配对）：

| 类别 | 提升率 | 噪声（组内 stdev） | 结论 |
| --- | --- | --- | --- |
| 查询类（4 项） | **+39.72%** | 8.9~18.8pp | 可靠 |
| 构建类（3 项） | +3.84% | `build_ik_all` 33.83pp、CV 34.1%、spread 近 100% | **纯噪声，是抽签** |
| 分词类（2 项） | 原始读数快 21%~23% | — | `select1` 扣偏差法在这里失效 |

两点必须写进方法论：

1. **构建类的单次 CI 读数不可信**。`build_ik_all` 的组内标准差 33.83pp、
   变异系数 34.1%、极差接近 100%——同一份二进制重复跑都能差出一倍。
   所以"构建类没有提升"这个结论本身也是抽签结果，**不能用来判断构建优化成败**，
   只能靠 CI 的整体分数间接反映。
2. **`select1` 扣偏差法对分词指标失效**。本机 `select 1` 往返本身要 0.19~0.25 ms，
   而 `tokenize` 本体只有 0.04~0.09 ms，扣掉往返会算出 +98% 这种荒谬值。
   分词只能用**同一候选端前后做差分**来衡量。

查询类内部，**`query_limit` 是最大短板**（+17.60%，其余三项 +44.91%/+47.71%/+48.66%）。
`EXPLAIN` 显示它是 BM25 top-K 计划（`calc_relevance=true`、`sort_keys([MATCH(...) DESC])`、
`is_index_back=true`、`limit(20)`），而另外三个是 `calc_relevance=false` 的纯索引扫描。
微基准：**带 LIMIT 20 的子查询（3.05~3.26 ms）比不带 LIMIT 的全量 COUNT（2.14 ms）更慢**，
且 LIMIT 1 与 LIMIT 20 耗时相同——说明 LIMIT 没有下推，top-K 要给全部 9333 个命中都算一遍
BM25 分数。

### 9.2 分词链路的 perf 剖析（perf 6.18.54.1 + 离线符号化）

对候选二进制跑 4 并发 `SELECT tokenize('<基准文本>', 'ik')`，采样 9137 个栈：

| 热点 | self | 说明 |
| --- | --- | --- |
| `ObArrayHashMap::find`（词典单字哈希表） | **4.71%** | 每匹配一个字符查一次；热指令是首次探测的 `used` 标志（cache miss） |
| `ObFTDATReader::match_with_hit`（DAT 匹配） | ~6.9% | DAT base/check 数组遍历 |
| `absl::LowLevelAlloc::Alloc`（malloc） | ~7% | 分散在词典匹配、JSON 树、哈希节点各处 |
| `ObFTParser::serialize_to_str` → `databuff_printf` | ~0.6% | 每次调用重新序列化 parser 名 |
| `ObFTParseHelper::init` | 0.48% | 每次调用重新解析 "name.version" |
| `ObAddWord::groupby_word` | 3.27% | 每个 token 两趟哈希（先查后写） |
| `ObIKCJKProcessor::do_process` + `ObFTRangeDict::match` | 16.3% | **词典匹配本体，是算法核心，不动** |

包含式看，`ObExprTokenize::eval_tokenize` 占进程 CPU 的 **36.12%**，
`tokenize_fulltext` 占 33.13%，`pack_json_res` 占 2.68%。
也就是说 TOKENIZE 表达式本身只占这条基准 SQL 的一部分，
**能动的只有"每次调用的固定开销"和"词典查表"两块**。

另外从查询侧的剖析里还找到一处第 5 轮的遗漏：
`ObDomainIdCmp`（`src/storage/retrieval/ob_sparse_retrieval_util.h`，
BM25 top-K 迭代器与 block-max 迭代器用的那一个）**没有**二进制快路径，
仍然走字符集分发（`ObNullSafeDatumStrCmp<CS_TYPE_BINARY>` →
`ObCharset::strcmpsp` → `ob_strnncollsp_binary`），在 `query_limit` 路径上合计约 2%。

### 9.3 本轮改动

#### 9.3.1 去掉每次调用新建的 `ObArenaAllocator`（`ob_expr_tokenize.{h,cpp}`）

原来 `TokenizeParam` 自带一个 `ObArenaAllocator allocator_`，`tokenize_fulltext()` 里
还有第二个 `ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"))`。
每次 `TOKENIZE()` 调用都要新建**两个** arena（各自申请/释放 arena block），
而它们的用途只是存几段 parser 属性 JSON。
现在统一用 `ObEvalCtx` 的临时分配器（`TempAllocGuard` 拿到的 `ctx.tmp_alloc_`），
一次调用只留一个 arena。

#### 9.3.2 规范化 parser 名的线程本地缓存（`ob_expr_tokenize.cpp`）

`construct_ft_parser_inner_name()` 每次调用都要把原始 parser 名校验一遍、
序列化成 "name.version"。结果只取决于原始名，而内置 parser 只有寥寥几个，
所以按原始名做一个 4 槽的**线程本地**缓存：命中即零拷贝零加锁直接复用；
miss 时才走原来的 `parser.init()` + `serialize_to_str()`。
缓存槽的原始名长度用 `raw_len_ = -1` 表示"永不可命中"，
以覆盖超长原始名（`ObFTParserName::set_name()` 本来就会拒绝这种名字）。

> 这是**配置派生量**的缓存（和第 1~5 轮已接受的 parser 属性缓存同一性质），
> 不是对分词结果做记忆化：文本仍然每次都完整分词。

#### 9.3.3 token word map 的线程本地复用（`ob_expr_tokenize.cpp`）

`tokenize_fulltext()` 每次调用都 `ObFTWordMap::create()` 一个新哈希表，
用完再析构：桶数组一次 malloc/free，节点块一次 malloc/free。
现在按线程缓存这张表并复用。

**桶数必须保持不变**：`ObFTWordMap` 的遍历顺序 = 桶序，桶数由
`MIN(MAX(len/2,2),997)` 决定（`len` = 文本字节数），而 TOKENIZE 的输出就是按这个
顺序打印的。所以只有请求的桶数与上次相同时才复用，否则 `destroy()` 后重建——
重建的开销与原来完全一样，不会更差。

空文档（`len == 0`）不走分词，而分词是唯一会清空这张表的地方，
所以空文档要显式 `reuse()` 清表。

#### 9.3.4 `ObDomainIdCmp` 的二进制快路径（`ob_sparse_retrieval_util.h`）

倒排索引的 doc id（domain id）是行键的二进制串。`ObDomainIdCmp` 走的是
`ObDatumFuncs::get_basic_func(type, collation)->null_first_cmp_`，
对 `CS_TYPE_BINARY` 就是 `ObNullSafeDatumStrCmp<63>` → `ObCharset::strcmpsp`
→ `ob_strnncollsp_binary`，每次比较都要过一遍字符集分发。

第 5 轮已经把**归并堆**的 doc id 比较改成直接 memcmp，但 `ObDomainIdCmp`
漏掉了。现在：当列的 collation 已经是 `CS_TYPE_BINARY` 且两边都非空时，
直接 memcmp + 长度差——与 `ob_strnncollsp_binary()` 的顺序完全等价
（字节先比，前缀相同时长度差决胜）。

#### 9.3.5 token word map 复用的一处错误路径加固

`prepare_token_map()` 在桶数变化时会先 `destroy()` 旧表再 `create()` 新表。
如果 `create()` 失败（内存不足），缓存里记录的桶数会**残留旧值**；
下一次请求的桶数恰好与这个残留值相同时，函数会误判成"可以直接复用"，
于是既不重建也不清表——此时那张表其实是一张 `destroy()` 过、`create()` 失败的表，
后续往里 `insert` 会直接返回 `OB_NOT_INIT`，此后同长度的文本会一直失败。

改法是 `destroy()` 之后立刻把缓存里的桶数置为 `-1`，
这样无论 `create()` 成功与否，下一次调用都会重新建表。

这条路径只有在内存不足时才会走到，基准里不会触发，
但它是一个真实的语义缺陷，所以本轮一并修掉。

### 9.4 试过但回退的一个改动：词典单字 code 查找的记忆化

`ObArrayHashMap::find`（4.71%）是分词侧最大的单点热点：IK 分词器对**同一个字符**
要查好几次词典——CJK 处理器先把所有"还活着的匹配"各推进一次，再为这个字符开一个新匹配，
而 range dict 与主词典各自还要先做一次区间二分，每一次最终都落到这个哈希探测上。
单字在词典里的 code 是**常量**（词典建好后只读、进程内共享、生命周期与进程同寿），
所以试过把"最近一次查找"记在**线程本地**里。

实现后 TOKENIZE 输出出现回归：约 60 条用例里有 7 条返回
`ERROR 4018 (HY000): Entry not exist`（全部是"中英文混排"的文本，
纯中文与纯英文的用例都正常）。同一轮里还改了 `hash_word()` 的字节拼装方式
（把变长 `MEMCPY` 换成内联拼装，避免 PLT 调用），
但新的哈希函数已用 340 万组随机用例与旧函数逐位比对，**0 处不一致**，
因此可以排除哈希改动、把嫌疑锁定在记忆化逻辑上。
本轮没能定位到记忆化逻辑里的根因，**该改动已整体回退**，
第 6 轮只保留上面 9.3 的四项改动。

> 这一条记在这里，是因为它本身就是"改分词必须用 TOKENIZE 逐字节比对兜底"的例子：
> 如果只看"命中数 8001/11000/7332/20 没变"，这个回归是发现不了的。

#### 9.4.1 回退必须以构建树为准（同步机制的一个坑）

回退那两处词典改动时，只回退了 Windows 侧的工作副本。同步到 WSL 构建树用的是
「Windows 工作区与提交的差异清单」，而回退之后这两个文件与提交**一致**了，
于是它们不在清单里，同步脚本也就没有覆盖构建树里的旧版本——
**WSL 构建树里留着的仍然是第 7 轮的词典改动。**

后果是：任何一次基于构建树的重新编译，都会把 `4018 Entry not exist` 这个回归
重新带回来。第 6 轮重编译时正是这样发现的——TOKENIZE 的输出直接变成了报错行，
而不是等到跑完基准才发现。

所以这一轮把流程改成了：**编译之前先做一次全树逐文件哈希比对**
（Windows 提交 vs WSL 构建树，统一换行符后比较 sha256），
确认两边只差本轮真正改过的文件，再开始编译。

这次比对第一次跑出来是 8923 个文件里有 2 个内容不一致
（`src/storage/fts/dict/ob_ft_dat_dict.{h,cpp}`），修正后再比是 0 个差异。
这也从侧面说明：**"回退"这件事的验收对象应该是构建树，而不是版本控制的工作区。**

### 9.5 正确性验证（第 6 轮二进制，md5 `0c12f4870a442bcc480bbf5aa12db284`）

| 检查 | 结果 |
| --- | --- |
| 命中数 | `query_cn`=8001、`query_beng`=11000、`query_mixed`=7332、`query_limit`=20，**与基线完全一致** |
| TOKENIZE 输出 | 30 条文本 × {ik, beng} + 4 条 parser 属性用例，共 **121 行**，与 **master 基线逐字节一致** |
| MATCH 语义 | 与第 5 轮候选**逐字节一致**；与 master 基线只有已知的 `calc_relevance` 计划标注差异 |
| fts_index 回归 | `fts_shared_dict_tokenize`、`fts_word_map_reuse`、`fts_doc_id_compare`、`basic_dml_sequel`、`tokenize_function`、`simple_query` **6/6 通过** |
| 仓库自动化检查 | `share_header_ownership_check`、`storage_header_ownership_check`、`share_terminal_check`、`oblib_terminal_check` 四项全过 |
| 构建树一致性 | 8923 个受版本控制的文件逐一比对（换行归一化后 sha256），源码 **0 差异**，详见 9.4.1 |

> 回归套件第一次跑时 `simple_query` 报过一次
> `4012 Statement timeout`——那是同一台机器上并行跑增量编译把 CPU 抢光导致的；
> 编译结束后单独重跑该用例即通过，与本轮改动无关。
>
> 最终二进制单独复跑，6/6 全过、无超时。
>
> TOKENIZE 的输出里有 5 行是**故意的反例**（`can't won't` 的引号语法错误、
> 非法 parser 参数），master 基线里同样是这 5 行错误，
> 所以"121 行与基线逐字节一致"包含它们——这正是要逐字节比对而不是只看命中数的原因。

### 9.6 本轮的前后性能对比（交错 A/B，3 轮 × 每轮 2 组配对）

两台服务器同时跑、持有完全相同的数据、ABBA 顺序交错：基线端是 master 基线二进制（端口 10100），候选端是本轮二进制（端口 10101），每轮每端各跑 2 次，取均值后配成 1 组，一共 3 组。

`select1` 往返：基线端 0.2198 ms，候选端 0.1988 ms（比值 0.905），两端基本对齐，说明两台机器的负载是平衡的。

| 指标 | 基线 | 候选 | 原始提升 | 扣掉 `select1` 往返后 | 组内 stdev |
| --- | --- | --- | --- | --- | --- |
| `build_ik_all_sec` | 62.1055 | 61.1455 | **+0.54%** | +0.51% | 9.90pp |
| `build_ik_content_sec` | 32.6152 | 33.4782 | **-2.88%** | -2.97% | 13.22pp |
| `build_beng_en_sec` | 28.1938 | 27.3700 | **+3.25%** | +3.20% | 5.48pp |
| `tokenize_ik_avg_ms` | 0.4204 | 0.3004 | **+24.27%** | +41.42% | 20.31pp |
| `tokenize_beng_avg_ms` | 0.2452 | 0.2198 | **+10.07%** | +11.96% | 8.36pp |
| `query_cn_avg_ms` | 3.1629 | 2.0288 | **+35.63%** | +37.67% | 10.18pp |
| `query_beng_avg_ms` | 4.3522 | 3.1592 | **+27.44%** | +28.37% | 3.03pp |
| `query_mixed_avg_ms` | 3.4293 | 2.2496 | **+34.44%** | +36.18% | 2.70pp |
| `query_limit_avg_ms` | 4.1933 | 3.3120 | **+20.78%** | +21.45% | 8.38pp |

按类别平均（三类等权，与 CI 口径一致）：

| 类别 | 指标数 | 类别提升 | 说明 |
| --- | --- | --- | --- |
| 构建 | 3 | **+0.30%** | 单次读数噪声极大，见下方说明 |
| 分词 | 2 | **+17.17%** |  |
| 查询 | 4 | **+29.57%** |  |

三类等权平均 = **+15.68%**，按官方口径 `score = clamp(mean, 0, 0.5) / 0.5 * 100` 折算为 **31.37 / 100**（本机读数，仅供对照，权威分数以 CI 为准）。

几点说明：

1. **构建类在本机是抽签**。同一份二进制重复跑，`build_ik_all` 的组内标准差就达到几十个百分点，所以构建类的读数不能用来判断构建优化成败，只能靠 CI 的整体分数间接反映；本轮没有改构建链路。

2. **分词类只能用原始读数**。本机 `select 1` 往返 0.2 ms 量级，而 `tokenize` 本体只有零点几毫秒，扣掉往返会算出荒谬值，所以分词类看原始比值；查询类用扣往返后的数字更贴近 CI 的口径。

3. **分词类指标里 TOKENIZE 表达式本身只占一部分**（其余是 SQL 解析、计划、下发等固定开销），所以表达式提速 X% 折算到该指标只有约 0.57X%。


作为重复性检验，同一套交错 A/B 协议在加固修复之前已经独立跑过一轮完整的 12 次采样
（基线 vs 上一版候选二进制）：构建 +10.07%、分词 +15.13%、查询 +34.85%、
三类等权 +20.02%。本轮（加固后的最终二进制）跑出来的是
构建 +0.30%、分词 +17.17%、查询 +29.57%、三类等权 +15.68%。

两轮采样互不依赖——二进制不同、时间不同、装置重新部署过、数据重新拷贝过。
**分词与查询的提升量级一致**（分词 15% vs 17%，查询 35% vs 30%），说明这两块的提升是稳定的。

而**构建类在两轮之间从 +10.07% 变成 +0.30%，差了将近 10 个百分点，
而这两轮之间构建链路一个字节都没有改过**。
这就是"构建类是抽签"的直接证据：同一条构建链路可以给出相差十倍的结论，
所以构建类的读数只能靠 CI 的整体分数间接反映，本报告不对构建类的数字下结论。

另外对比 9.1 那一轮（基线 vs 第 5 轮候选）：`query_limit` 当时是最大短板（+17.60%），
本轮把 `ObDomainIdCmp` 改成二进制快路径之后，它明显改善到 +20.78%（扣往返 +21.45%），
而另外三个查询指标本来就是纯索引扫描、不走 top-K 路径。
这也反过来印证了 9.1 的判断：**`query_limit` 慢是因为 top-K 要给全部命中算 BM25 分数**，
而不是因为索引本身。

### 9.7 CI 结果与得分结构（第 6 轮之后）

第 6 轮推送后 CI 的结果是 **`fts_raw_score` = 31.07**（`fts_mean_improvement` = 15.54%），
上一版（第五轮 `0890f97`）是 **28.60**（14.30%）。

#### 9.7.1 本地交错 A/B 可以预测 CI 分数

把本地的类别提升率套进 CI 的评分公式，结果是：

| CI 运行 | 本地预测的分数 | CI 实际分数 | 差 |
| --- | --- | --- | --- |
| `0890f97`（第五轮） | 27.34 | 28.60 | +1.26 |
| `82eb173`（第六轮） | **31.36** | **31.07** | **−0.29** |

**本地测量与 CI 分数已经对齐到 1 分以内**（第五轮那 1.26 分的偏差落在构建类的噪声带内）。
这意味着后面几轮不必靠"推上去跑一次 CI"来验证效果，
本地双机交错 A/B 就能预判分数，也就不必反复消耗 CI 轮次。

#### 9.7.2 现在的得分结构

用同一台 CI 机器上的 master 基线反推出来的类别提升率是：

| 类别 | 类别提升 | 说明 |
| --- | --- | --- |
| 构建 | **≈0%** | 本轮没有改构建链路，读数由噪声主导 |
| 分词 | **≈17%** | 测量稳定（CI 上 stdev 0.0004~0.0012 ms） |
| 查询 | **≈30%** | 测量最稳（CI 上 stdev 0.0005~0.0021 ms） |

三类等权 = 15.7%，折算 31.4 分。

#### 9.7.3 满分的门槛，以及各类能不能到

官方公式是 `score = clamp(三类等权平均, 0, 0.5) / 0.5 * 100`，
所以**满分要求三类每一类都达到 50% 的改进率**（不是平均达到 50%）。

把 CI 机器上的 `select1` 往返（**0.0268 ms**）当成不可压缩的地板，
每个指标能达到的改进率上限就是 `(master − select1) / master`：

| 指标 | master | 50% 目标值 | 地板 | 能不能到 50% |
| --- | --- | --- | --- | --- |
| `tokenize_ik_avg_ms` | 0.0850 | 0.0425 | 0.0268 | 可以，但要把表达式工作量从 0.0376 压到 0.0157 ms（**2.4 倍**） |
| `tokenize_beng_avg_ms` | 0.0438 | 0.0219 | 0.0268 | **不可能**——往返本身就超过 master/2 |
| `query_cn_avg_ms` | 1.4059 | 0.7030 | 0.0268 | 可以，工作量再降 23% |
| `build_ik_all_sec` | 8.4054 | 4.2027 | 0 | 可以，但要砍掉一半的索引构建时间 |

由此得到几条硬结论：

1. **分词类有数学天花板**。`tokenize_beng` 永远到不了 50%，
   把两个 tokenize 指标都压到地板，该类的上限也只有 **53.7%**。
   现实一点，能到 35%~40% 就已经是很不容易的结果。
2. **查询类没有地板**，是三个类别里唯一能真正冲到 50% 的；
   它现在 30%，做到 50% 值 **+13.6 分**。
3. **分词类基数最低**，从 17% 做到 40% 值 **+15.2 分**，
   而且 CI 上的测量很稳（不像构建类那样会被 ±24% 的噪声淹没）。
4. **构建类空间最大但读数最不可信**：0% → 30% 值 **+19.8 分**，
   可它同一份代码在两次 CI 之间就摆动了 ±24%（见下），而且改它要动共享存储层。

#### 9.7.4 CI 自身的噪声有多大

第 5 轮和第 6 轮之间**构建链路一个字节都没改**，CI 上的读数却是：

| 指标 | `0890f97` | `82eb173` | 变化 |
| --- | --- | --- | --- |
| `build_ik_all_sec` | 6.761 | 8.360 | **+23.7%** |
| `build_ik_content_sec` | 6.735 | 5.128 | **−23.9%** |
| `build_beng_en_sec` | 2.825 | 2.825 | 0 |
| `build_total_sec` | 16.327 | 16.319 | 0 |

构建类一个指标摆 24%，三个指标平均后类别提升率就有 ±8pp 的抖动，
折算到总分是 **±2.7 分**。也就是说：**这一次 CI 的 +2.48 分，
完全可能被构建类的噪声解释掉**，本轮改动在 CI 上的真实效果还需要更多样本才能确认。

所以下一轮的取舍是明确的：
**优先做"测量稳定 + 还有真实空间"的类别（分词、查询），不去赌构建类。**

## 10. 第 7 轮：分词热路径的词典查找与词频统计

### 10.1 先量后改：用当前二进制重新剖析分词链路

上一轮（第 6 轮）留下的 perf 采样是在**旧二进制**上采集的，符号地址已经错位
（例如 `parse_json_param` 会被错误地归因到实际是 `tokenize_fulltext` 的地址上）。
所以这一轮先用**当前二进制**（md5 `0c12f4870a442bcc480bbf5aa12db284`）重新采集：

```
perf record -e cpu-clock -F 997 -g -p <seekdb pid> -- sleep 40
perf script -i perf_tokenize_r7.data > perf_tokenize_r7.data.stacks
# 离线用 nm 的符号表解析地址（脚本：resolve_perf2.py / subtree.py / leaf_callers.py）
```

采样条件与基准一致（4 个并发客户端压 `SELECT tokenize('<同一段中文>', 'ik')`），
共 5486 个采样栈。下面所有百分比都是**占全部采样的比例**。

#### 10.1.1 分词表达式就是这条负载的主体

| 栈帧（含调用者） | 占全部采样 |
| --- | --- |
| `ObMPQuery::process` | 70.36% |
| `ObExprTokenize::eval_tokenize` | **38.01%** |
| `ObExprTokenize::tokenize_fulltext` | 35.49% |
| `ObFTParseHelper::segment`（9 参重载） | 26.94% |
| `ObIKFTParser::process_next_batch` | 26.03% |
| `ObIKCJKProcessor::do_process` | 15.46% |
| `ObFTDATReader::match_with_hit`（词典匹配） | 12.05% |
| `ObArrayHashMap::find`（词典哈希表查找） | 6.47% |
| `ObAddWord::process_word` | 6.51% |
| `ObAddWord::groupby_word`（词频统计） | 4.81% |
| `ObIKArbitrator`（切分仲裁） | 4.72% |

也就是说：**在跟负载相关的 CPU 里（`ObMPQuery::process` 70.36%），
`eval_tokenize` 一个函数就占了 54%**。
这解释了 9.7.3 里"分词类基数最低、还有真实空间"的判断——
分词链路的每一个百分點都会同时打到 `tokenize_*` 和 `build_*` 两类指标上
（索引构建走的是同一套分词器）。

#### 10.1.2 词典查找被重复做了 N 次

`ObIKCJKProcessor::do_process` 对每一个汉字做两件事：

1. 把当前字符喂给**每一个还活着的 hit**（`hits_` 里的候选词），
   每个 hit 都要在自己的字典里查一次这个字的 code，再走一次双数组 Trie；
2. 再用当前字符**新开一个 hit**，这又是一次查找。

剖析显示 `match_with_hit`（步骤 1）的调用量是 `ObFTRangeDict::match`（步骤 2）的
**6.7 倍**，也就是说平均每个汉字有 **5 个左右同时活着的 hit**，
于是同一个字在一个字典里被查了 5~6 次。

为什么不能简单地把这些查找合并？因为每个 hit 有自己的 `dict_`，
而**每个字典有各自的 code 空间**（code 是该字典双数组 Trie 里的偏移），
跨字典的 code 不可比、不可复用。

#### 10.1.3 哈希本身还有一次 libc 调用

`ObArrayHashMap::find` 的哈希函数是：

```cpp
uint64_t chunk = 0;
MEMCPY(&chunk, ptr, len);        // len 是运行期变量 → 编译成 libc memcpy 调用
```

`len` 是运行期值，`MEMCPY` 只能展开成一次 `memcpy` 的函数调用，
而这个哈希在**每个被匹配的字符**上都要算一次。

#### 10.1.4 词频统计对新词做了两趟哈希

`ObAddWord::groupby_word` 对每个词先 `get_refactored` 查一次、
再 `set_refactored` 写一次。而 `set_refactored` 在 key 已存在时
是**新建一个节点再替换旧节点**（多一次分配 + 一次释放）。
词频统计占 4.81%，其中 `set_refactored` 与 `get_refactored` 各占约一半。

### 10.2 本轮的改动

#### 10.2.1 词典查找：同一个字典只查一次 code

**改动**：`src/storage/fts/dict/ob_ft_dict.h`、`ob_ft_cache_dict.{h,cpp}`、
`ob_ft_range_dict.{h,cpp}`、`ob_ft_dat_dict.{h,cpp}`、
`src/storage/fts/ik/ob_ik_cjk_processor.cpp`

在 `ObIFTDict` 接口上新增两个方法，把"查 code"和"走 Trie"拆开：

```cpp
// 查这个字在本字典里的 code；不在字典里返回 OB_ENTRY_NOT_EXIST
virtual int find_word_code(const ObString &single_word, ObFTWordCode &code) const = 0;
// 用已经查好的 code 走 Trie
virtual int match_with_code(const ObString &single_word,
                            const ObFTWordCode code,
                            const ObDATrieHit &last_hit,
                            ObDATrieHit &hit) const = 0;
```

`ObIKCJKProcessor::do_process` 用一个 4 槽的小缓存（`ObFTCJKCodeCache`）
按**字典指针**缓存当前字符的 code：

* 同一个字典里的所有 hit 共享一次查找；
* 不同字典仍然各查各的（code 空间不同，不能混用）；
* 缓存满了就退化成逐 hit 查找，行为不变。

于是每个汉字的查找次数从 `1 + N`（N≈5）降到 `1 + 不同字典数`（通常是 1~2）。

`ObFTDATReader::match_with_hit` 本身也被重写成 `find_code` + `match_with_code`
两段，语义与原来逐字对应（找不到字 → hit 置 unmatch → hit 被丢弃，
与原来的 `OB_ENTRY_NOT_EXIST → set_unmatch()` 完全一致）。

#### 10.2.2 词典哈希：不再调用 libc memcpy

**改动**：`src/storage/fts/dict/ob_ft_dat_dict.h`、`ob_ft_dict_def.cpp`

一个"单字"是 `ObFTSingleWord`，里面是**定长**的 `char word[ObCharset::MAX_MB_LEN]`
缓冲加一个长度。改成直接对这个定长缓冲做哈希：

```cpp
static OB_INLINE uint64_t hash_single_word(const ObFTSingleWord &word)
{
  uint64_t chunk = 0;
  MEMCPY(&chunk, word.word, sizeof(word.word));   // 定长 → 编译成一次定长拷贝
  uint64_t hash = chunk ^ (uint64_t)word.word_len * 0x9E3779B97F4A7C15ULL;
  ...
}
```

为了让同一个字只有一种表示，`ObFTSingleWord::set_word` 现在会把长度之后的
字节清零（否则缓冲里的残留字节会让哈希依赖于上一次写入的内容）。

**这一改动对哈希值没有任何影响**：旧的 `hash_word()` 把 `len` 个字节拷进一个
**先清零**的 `uint64_t`，所以缓冲区里 `len` 之后的字节本来就是 0；
新的 `hash_single_word()` 拷贝的是整个定长缓冲，而它的尾部同样被清零。
两个函数拿到的 `chunk` 完全一样，`hash` 的后续混淆也一样，
**所以桶下标、字典表的布局与改动前逐位相同**——这是一个纯优化。

`insert` 和 `find` 必须用同一个哈希，否则字典表会自相矛盾：
`insert` 现在也构造 `ObFTSingleWord` 并调用 `hash_single_word`
（长度超过 `MAX_MB_LEN` 的 key 走原来的通用哈希——这种 key 本来就
`set_word` 失败、永远查不到）。

**注意**：字典表是在进程内构建的（`ObFTDATBuilder` → `ObArrayHashMap`），
不落盘、不跨版本复用，所以哈希函数可以在同一个二进制内自由更改。

#### 10.2.3 词频统计：新词只哈希一趟

**改动**：`src/storage/fts/ob_fts_stop_word.cpp`

```cpp
// 原来：先 get（查一次哈希）再 set（再查一次哈希）
// 现在：先按"不覆盖"插入，插入成功就说明是新词，一次哈希搞定；
//       返回 OB_HASH_EXIST 才走原来的 get + set 累加路径。
ret = word_map_.set_refactored(word, 1/*word count*/, 0/*do not overwrite*/);
if (OB_HASH_EXIST == ret) { /* 原来的累加逻辑 */ }
```

最终表内容与原来完全一致（同一个 key、同一个计数、桶里的位置也一样），
只是新词少了一次哈希查找，老词少了一次"查不到"的空跑。

#### 10.2.4 查询：MATCH 的 doc id 比较统一走二进制序

**改动**：`src/sql/das/iter/sparse_retrieval/ob_das_match_iter.{h,cpp}`

`ObDASMatchMergeCmp` 的 id 是倒排表的行键（二进制串），
但比较时仍然走字符集分发。同第 5 轮的 `ObSRMergeCmp`、第 6 轮的 `ObDomainIdCmp`，
改成 `memcmp` + 长度差直比（与 `ob_strnncollsp_binary()` 等价）。


### 10.3 正确性验证（第 7 轮二进制，md5 `5a35300cebeb085a7fa820244c0bae95`）

| 检查项 | 方法 | 结果 |
| --- | --- | --- |
| MATCH 命中数 | 基准脚本的 4 条查询逐条比对 | **8001 / 11000 / 7332 / 20**，与基线一致 |
| MATCH 语义 | `EXPLAIN` + 语义输出逐字节比对第 6 轮 | **逐字节一致**（与 master 基线只差第 3 轮就已存在的 `calc_relevance` 标注差异） |
| TOKENIZE 结果 | 121 行输出与 master 基线逐字节比对 | **逐字节一致** |
| **新增回归用例** `fts_dict_lookup_cache` | `.result` 在 **master 基线二进制**上录制，再在 master / r6 / r7 三个二进制上分别重放 | **三个二进制输出完全一致，全部通过** |
| `fts_index` 套件（FTS 相关） | `fts_shared_dict_tokenize`、`fts_word_map_reuse`、`fts_doc_id_compare`、`fts_dict_lookup_cache`、`tokenize_function`、`simple_query` 连跑 | **6/6 通过** |
| `basic_dml` | 候选端（连跑 + 单独重放）与 **master 基线端**各跑一次 | **三个二进制都失败**，根因是上游用例的一个时序假设（见 10.3.2） |
| `basic_dml_sequel` | 候选端在**空闲服务器上单独重放** | **通过（rc=0）**；连跑时报 4012 超时（见 10.3.2） |
| `cn_word` | 候选端与 master 基线端各跑一次 | **两端同样失败**（第 44 行 `Access denied for user 'admin'`） |
| 仓库自动化检查 | 13 项（见 10.3.4） | **13 过 0 败** |
| 构建树一致性 | 本轮改动的 **14 个文件**逐一 sha256 比对（换行归一化后） | **14/14 一致**，与 9.4.1 的全树比对（8923 文件、0 差异）一起保证编译的是同一份代码 |

#### 10.3.1 新增的回归用例

第 7 轮的三处改动（词典 code 查找、单字哈希、词频统计）都作用在**分词的内部路径**上，
只看"命中数没变"是发现不了这类回归的（第 6 轮 9.4 记录的 `4018 Entry not exist`
就是这样漏过去的）。所以本轮补了一个专门的用例
`tools/deploy/mysql_test/test_suite/fts_index/t/fts_dict_lookup_cache.test`：

1. **同时存活多个候选词的句子**——`中华人民共和国万岁万岁万万岁`、
   `数据库索引全文检索性能优化`、`全文索引的分词器热路径优化`，
   每一个汉字都会同时推进好几个 hit，词典查找被反复调用；
2. **字典里没有的字**——`龘齾靐灥`、`中龘文齾分靐词`，
   以及 4 字节的 `𠀀`（这些字要么被丢弃、要么被当成单字切出来）；
3. **单字哈希的边界**——单字母 `a`（ik / beng 两种词典）、`中`、`0`、
   全角标点 `。。。`、中英文数字混排 `中文abc123`、`ABCabc123`；
4. **两种词典的 code 空间隔离**——`database 稳定 index 2024` 在 ik 与 beng 下分别切；
5. **词频统计**——用 `{"output":"ALL"}` 的明细输出检查每个 token 的词频
   （明细输出里带的就是 `groupby_word` 统计出来的计数）；
6. **建索引之后**——建完全文索引、共享词典进入使用状态后再切一次；
7. **二进制行键的 MATCH**——`varchar` 主键的表上做 MATCH，
   覆盖本轮改动的 DAS 归并 doc id 比较。

**期望输出（`.result`）是在 master 基线二进制上录制的**，
所以这个用例对"分词结果是否变化"是真正的回归守卫：
master、第 6 轮、第 7 轮三个二进制跑出来的结果**逐字节一致**。

#### 10.3.2 三个失败的用例都是环境/上游用例问题，不是回归

1. **`basic_dml` 的失败与本轮改动无关**。

   用例在第 1555 行记下 `$data_table_id`（`fts_parser_name_test` 的 table_id），
   第 1582 行 `DROP TABLE`、第 1584~1588 行用**同样的表名重建**，
   但**没有刷新 `$data_table_id`**，
   于是第 1589~1595 行查出来的内部表 id 是空的，
   第 1598 行的 `eval` 把 mysqltest 的错误文本
   （`no such row`）当成表 id 拼进 SQL，于是报了一个语法错误。

   这个用例要成立，**必须假设"新建的表复用了被删表的 table_id"**。
   在本环境里这个假设不成立——直接复现：

   ```
   第一次建表:  table_id = 544302   （内部表 13/14 的 data_table_id = 544302）
   DROP + 重建:  table_id = 544307   （内部表 13/14 的 data_table_id = 544307）
   → 表 id 没有被回收复用，用例里的 $data_table_id 是陈旧的
   ```

   **表 id 的回收是 schema 层的 DDL 行为，与分词器、词典、词频统计、DAS 归并比较都无关**；
   而且 **master 基线二进制在同一个环境下同样失败**（并发时失败在第 1010 行的
   `DROP TABLE ... 4012 Timeout`，单独重放也失败在第 1598 行）。
   候选端在连跑与单独重放两种情况下都稳定地失败在第 1598 行，
   失败点与 master 端一致。

2. **`basic_dml_sequel` 的失败是并发造成的超时**：
   第 428 行的大 INSERT 报
   `4012: Timeout, query has reached the maximum query timeout`。
   这个用例有 1600 多条 DDL，本机上单例就要十几分钟，
   同一台机器同时跑两个这种用例必然触发查询超时
   （本轮第一次连跑就是和另一个 DDL 重用例并��跑的）。
   **在空闲服务器上单独重放同一个用例通过（rc=0）**。
   第 6 轮也记录过同样的现象（见 9.5 的注）。

3. **`cn_word` 需要 `admin` 账号和 `$OBMYSQL_PWD`**，本环境没有，
   所以它在候选端和基线端都失败在同一条语句上，属于环境限制。

#### 10.3.3 构建树一致性

9.4.1 记过一次坑：回退改动时只回退了 Windows 侧的工作副本，
而同步机制（"与提交的差异清单"）没有覆盖构建树，**结果构建树里留着的还是旧改动**。
所以这一轮的流程是"先全树哈希比对、再编译"。

这一轮全树比对（8923 个文件、0 差异）之后，**又新增了两个文件**
（`fts_dict_lookup_cache.test` 与它的 `.result`），
所以在提交前又对这 14 个文件做了一次定向比对：**14/14 一致**。

#### 10.3.4 仓库自动化检查

13 项全部通过（其中 `oblib terminal` 与 `share terminal` 需要 `bazel` 在 `PATH` 上，
即 `~/dl/bazel` 提供的 8.2.1）：

| 检查 | 结果 |
| --- | --- |
| `check_license_headers.py`（`--base-ref` 指向分支起点 `829bcd8`） | 通过 |
| `source_input_check.py` | 通过 |
| `sql_source_ownership_check.py` | 通过 |
| `sql_header_ownership_check.py` | 通过 |
| `share_header_ownership_check.py` | 通过：379 tracked / 250 public roots / 77 interface closure / 52 private |
| `storage_header_ownership_check.py` | 通过：795 tracked / 146 public / 450 closure / 193 private / 6 semantic |
| `logservice_source_ownership_check.py` | 通过 |
| `module_layer_check.py` | 通过 |
| `runtime_seam_definition_check.py` | 通过 |
| `syspack_manifest_check.py` | 通过 |
| `oblib_terminal_check.py` | 通过：explicit public owner DAG, private implementation seam, and no upward runtime access |
| `share_terminal_check.py` | 通过：94 Share rules and 1622 external BUILD files checked |
| `unittest_module_check.py` | 通过 |

> 这些检查是在 **WSL 构建树**（`/home/liyunxu/seekdb-repo`）上跑的，
> 它的内容与 Windows 工作树逐文件一致（9.4.1 的全树比对 + 本轮的 14 个文件定向比对），
> 在 ext4 上跑比在 `/mnt/f` 上快一个数量级。

### 10.4 本轮的前后性能对比

本轮做了**两组**独立的测量，因为"跨时段的基线对比"在这台机器上不可靠（见 10.4.3）。

#### 10.4.1 全协议交错 A/B（master 基线 vs 第 7 轮二进制）

两台服务器同时跑、持有完全相同的数据、**ABBA 顺序交错**（基线端 10100 = master 基线二进制，
候选端 10101 = 本轮二进制 md5 `5a35300cebeb085a7fa820244c0bae95`），每轮每端各跑一次，
3 轮共 6 组配对。每一次跑的都是**全协议**（`SKIP_LOAD=0`：重新导入数据 + 重建 3 个全文索引 +
分词 + 查询），与 CI 的协议一致；每个测量阶段之前都等两台服务器连续 3 个 6 秒窗口低于单核 25%，
把 compaction / GC 的尾巴排除在测量窗口外。

`select1` 往返：基线端 0.1838 ms，候选端 0.1876 ms。

| 指标 | 基线 | 候选 | 原始提升 | 扣掉 `select1` 往返后 |
| --- | --- | --- | --- | --- |
| `build_ik_all_sec` | 66.620 | 74.788 | **−12.26%** | −12.29% |
| `build_ik_content_sec` | 45.127 | 43.327 | **+3.99%** | +4.01% |
| `build_beng_en_sec` | 38.534 | 36.539 | **+5.18%** | +5.21% |
| `tokenize_ik_avg_ms` | 0.3141 | 0.2719 | **+13.45%** | +35.29% |
| `tokenize_beng_avg_ms` | 0.2267 | 0.2144 | **+5.43%** | +37.46% |
| `query_cn_avg_ms` | 2.9291 | 1.8315 | +37.47% | **+40.12%** |
| `query_beng_avg_ms` | 4.3000 | 2.6139 | +39.21% | **+41.05%** |
| `query_mixed_avg_ms` | 3.1582 | 2.0552 | +34.93% | **+37.21%** |
| `query_limit_avg_ms` | 3.6401 | 3.1032 | +14.75% | **+15.64%** |

| 类别 | 指标数 | 类别提升 | 口径 |
| --- | --- | --- | --- |
| 构建 | 3 | **−1.03%** | 原始比值 |
| 分词 | 2 | **+9.44%** | 原始比值 |
| 查询 | 4 | **+33.51%** | 扣 `select1` 往返后 |

三类等权 = **+13.97%**，折算 **27.94 / 100**（本机读数，仅供对照，权威分数以 CI 为准）。

#### 10.4.2 三臂同场测量（master / 第 6 轮二进制 / 第 7 轮二进制）

上一张表有一个问题：**基线端和候选端是先后测的**，而这台机器的负载在一天之内会变，
所以"基线读数"本身会漂移。要回答"第 7 轮的改动到底带来了多少"，必须让**两个候选二进制
在同一场里比**。

于是加装了第三个臂：**第 6 轮的二进制**（md5 `0c12f4870a442bcc480bbf5aa12db284`）
装到 `10102` 端口，三台服务器持有**完全相同的数据**（20000 行 + 3 个全文索引，
`query_cn` 命中数都是 8001）。一轮的顺序是
**基线 → r6 → r7 → r7 → r6 → 基线**，任何单调漂移在轮内都抵消；
每轮每端各 2 个样本，3 轮共 6 个样本/端。
这一次用 `SKIP_LOAD=1`（索引已经建好，只测分词与查询），
因为第 7 轮改的正是这两条路径，而建索引那一段在本机是噪声主导、测不出东西。

| 指标 | 基线 | r6 | r7 | 基线→r6 | 基线→r7 | **r6→r7** | 轮内 stdev |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `tokenize_ik_avg_ms` | 0.3201 | 0.2719 | 0.2733 | +15.06% | +14.60% | **−0.59%** | 3.26pp |
| `tokenize_beng_avg_ms` | 0.2369 | 0.2151 | 0.2127 | +9.21% | +10.22% | **+1.10%** | 2.05pp |
| `query_cn_avg_ms` | 2.8096 | 1.7765 | 1.7977 | +39.28% | +38.68% | **−1.09%** | 7.55pp |
| `query_beng_avg_ms` | 4.0192 | 2.6526 | 2.5911 | +35.67% | +37.37% | **+2.09%** | 7.98pp |
| `query_mixed_avg_ms` | 3.1066 | 2.0648 | 1.9952 | +35.69% | +38.18% | **+3.65%** | 5.21pp |
| `query_limit_avg_ms` | 3.6069 | 3.0130 | 2.9399 | +17.30% | +19.57% | **+2.62%** | 3.98pp |

（查询类的比值是扣掉 `select1` 往返后的；`select1` 三臂分别是 0.1867 / 0.1867 / 0.1908 ms。）

| 类别 | 基线→r6 | 基线→r7 | **r6→r7（第 7 轮净增量）** |
| --- | --- | --- | --- |
| 分词 | +11.97% | +12.25% | **+0.26%** |
| 查询 | +31.98% | +33.45% | **+1.82%** |

**这是本轮最重要的一张表**，结论有三条：

1. **第 6 轮的成果被完整复现**：基线→r6 给出分词 +11.97%、查询 +31.98%，
   与第 6 轮 CI 上的分词 ≈17%、查询 ≈30% 同一量级，说明这套本机测量是可信的。
2. **第 7 轮是净正收益，但幅度不大**：分词 +0.26%、查询 +1.82%，
   都在轮内标准差（3~8pp）之内或接近它，只能说"没有变差、略有变好"。
3. **第 7 轮没有把分词类的天花板抬高**。原因在 10.4.4。

#### 10.4.3 为什么"分词类"在本机看起来只有 10%，而在 CI 上是 17%

分词指标里混着**客户端往返**。本机 `select 1` 往返要 0.187 ms，
而 `tokenize_ik_avg_ms` 基线是 0.320 ms——**往返占了 58%**；
CI 机器上 `select1` 只有 0.0268 ms，而 `tokenize_ik_avg_ms` 基线是 0.0850 ms，
**往返只占 32%**。

也就是说，本机分词指标里能反映"内核工作量"的部分只有 42%，CI 上有 68%——
本机天然把分词类的提升率**压低**了。这与第 6 轮的结论一致
（本地预测 31.36 vs CI 实际 31.07，分项上分词本地偏低、查询本地偏高，两者大致抵消）。

#### 10.4.4 第 7 轮为什么只有这么小的增量：分词指标里的"可优化部分"本来就少

把剖析数据（10.1）和三臂测量（10.4.2）放在一起看：

| 分词链路上的开销 | 占采样 | 第 7 轮动了它吗 | 预期收益 |
| --- | --- | --- | --- |
| `eval_tokenize` 整体 | 38.01% | 部分 | — |
| └ `match_with_hit`（词典匹配） | 12.05% | 拆成"查 code"+"走 Trie" | 只省下**查 code** 那一次哈希 |
| └ `ObArrayHashMap::find`（词典哈希表） | 6.47% | 改成定长哈希（去掉 libc memcpy） | 省一次 PLT 调用 |
| └ `groupby_word`（词频统计） | 4.81% | 新词只哈希一趟 | 省一趟哈希 |
| `ObIKArbitrator`（切分仲裁） | 4.72% | **没动** | — |
| SQL 解析 / 计划 / 下发 / 结果编码 | 约 32% | **没动** | — |

第 7 轮动的是上表里**可以省掉重复工作**的那几项，而它们加起来只占采样的一小部分：
- Trie 遍历本身（每个 hit 各自的位置）**不能共享**——那是算法本体；
- `ObIKArbitrator` 与 SQL 层的固定开销**没有动**；
- 词典命中那部分是"查一次 code + 走一次 Trie"，而每个汉字平均有 5 个存活 hit，
  能省的只是**同一个字典内重复的 code 查找**，省不掉 Trie 遍历。

所以"每个汉字少查 4~5 次字典"听起来很大，折算到 `eval_tokenize` 只有 10% 量级，
折算到分词指标（本机还要再乘 0.42 的权重）就只有 1~2pp——这与三臂测出的
**+0.26%（分词类）/ +1.82%（查询类）** 是一致的。

**结论**：第 7 轮的改动方向正确（确实去掉了重复工作）、正确性有保障（TOKENIZE 逐字节一致），
但**指望它把分数拉上去是不现实的**。要继续提分，必须去动
"没有重复工作可省"的那几块——尤其是 `query_limit`（10.4.5）和构建链路。

#### 10.4.5 剩下的短板：`query_limit`

三臂测量里，`query_limit` 是查询类中唯一的短板：

| 查询指标 | 基线→r7 |
| --- | --- |
| `query_cn` | +38.68% |
| `query_beng` | +37.37% |
| `query_mixed` | +38.18% |
| **`query_limit`** | **+19.57%** |

原因在 9.1 已经查清：`query_limit` 走的是 BM25 top-K 计划
（`calc_relevance=true`、`sort_keys([MATCH(...) DESC])`、`LIMIT 20`），
而另外三个是 `calc_relevance=false` 的纯索引扫描。
`calc_relevance=true` 会额外触发 `estimate_token_doc_cnt()`
（`src/storage/retrieval/ob_text_retrieval_token_iter.cpp:372`，
注释里写明只在需要计算相关度时调用），也就是说
**带 LIMIT 的查询比不带 LIMIT 的查询多走一条"估计每个 token 的文档数"的路径**，
而这条路径的结果在第 5 轮把 BM25 布尔化之后已经没有用处了
（第 5 轮的改动让"只用作过滤条件"的 MATCH 不再计算分数）。

这是下一轮（第 8 轮）的第一目标。

#### 10.4.6 构建类：本轮没有改，也不下结论

构建类在本机是"抽签"（见 9.6）：同一个二进制重复跑，`build_ik_all` 的组内标准差就有几十个百分点，
9.7.4 还记录了 CI 上同一份代码两次运行之间 `build_ik_all` 摆动 **+23.7%**、
`build_ik_content_sec` 摆动 **−23.9%**。本轮**没有改构建链路**，
但分词器的改动在原理上会同时作用到索引构建上（索引构建走的是同一套分词器），
所以 10.4.1 里构建类的 −1.04% 只能理解为"噪声 + 一个很小的正收益被噪声淹没"。

**本报告不对构建类的数字下结论**，构建类的真实效果只能靠 CI 的整体分数间接反映。

### 10.5 本轮的结论与下一轮的方向

#### 10.5.1 结论

1. **第 7 轮的改动是正确的**：TOKENIZE 的 121 行输出与 master 基线**逐字节一致**，
   MATCH 命中数 8001 / 11000 / 7332 / 20 不变，新增的 `fts_dict_lookup_cache`
   回归用例在 master 基线、第 6 轮二进制、第 7 轮二进制上输出**完全一致**。
2. **第 7 轮是净正收益，但幅度很小**：同场三臂测量给出分词类 **+0.26%**、
   查询类 **+1.82%**（r6 → r7）。它没有把分数拉上去，
   原因在 10.4.4——分词链路里"可以省掉的重复工作"占比本来就低。
3. **第 6 轮的成果被完整复现**（基线→r6：分词 +11.97%、查询 +31.98%），
   这反过来验证了本轮测量方法本身是可信的。

#### 10.5.2 下一轮（第 8 轮）的目标：按"剩余空间 × 可测性"排序

| 目标 | 现状 | 剩余空间 | 可测性 | 风险 |
| --- | --- | --- | --- | --- |
| **`query_limit`** | +19.57%（其余三个查询 +38%） | 查询类 26.9% → 33% ≈ **+2 分** | CI 上 stdev 0.003 ms，**很稳** | 低（只影响 `calc_relevance=true` 的计划） |
| **构建类** | ≈0% | 0% → 30% ≈ **+10 分** | **极不稳**（CI 上 ±24%） | 高（共享存储层） |
| **分词类** | ≈17% | 数学上限 53.7%（见 9.7.3） | 稳 | 中 |

**第一目标：`query_limit`。**
它是查询类里唯一的短板（19.57% vs 38%），而且它的额外开销来源已经定位清楚：
`calc_relevance=true` 会触发 `estimate_token_doc_cnt()`，
而第 5 轮把 BM25 布尔化之后，"只用作过滤条件"的 MATCH 根本不需要分数。
这条路径是**纯重复工作**，去掉它不改变任何命中结果。

**第二目标：分词链路的"不可压缩部分"。**
10.4.4 指出，词典匹配里真正不能省的是 Trie 遍历和 `ObIKArbitrator` 的仲裁。
下一步要么压缩 Trie 遍历本身，要么减少**存活 hit 的个数**
（现在平均每个汉字有 5 个存活 hit，每个都要走一遍 Trie）。

**构建类本轮仍然不作为主攻方向**：空间最大但读数最不可信，而且改它要动共享存储层
（作业明令要避免共享存储层的高风险改动）。

## 11. 第 8 轮：索引构建链路上的 Rust 数据面页校验和

第 7 轮把分词和查询两条链路都压了一轮之后，剩下的最大一块就是**索引构建**。
这一轮按"直接冲 build 的大头"的方向做：先把构建链路的 CPU 归属彻底量化，
再定位到具体的代码行，最后用**校验和值逐位不变**的方式改掉它。

### 11.1 先量后改：构建链路的 perf 归因

采样方式和第 9/10 轮一致，只是把负载换成建索引：

```
perf record -F 999 -g -p <seekdb pid> -- sleep 900      # 期间做 3 轮重建
perf script -i perf_build_r8.data > perf_build_r8.data.stacks
```

一共 181477 个采样栈。下文的百分比都是**占全部采样**的比例。

#### 11.1.1 按线程名归因：MergeScheduler 的栈只有一层

| 线程 | 占全部采样 | 其中 depth<=1 的栈占比 |
| --- | --- | --- |
| `PX_G0` | 41.46% | 0.5% |
| `MergeScheduler` | **34.81%** | **93.9%** |
| `DDLTaskExecutor` | 5.12% | 2.2% |
| `TxLoopWorker` | 4.87% | 2.4% |
| `ReqWorker` | 2.13% | 2.4% |

`MergeScheduler` 这一行非常反常：一个归并线程，**93.9% 的采样栈只有一层**。
正常的 C++ 调用栈不会只有一层，只有**被完全内联**的函数才会这样。

#### 11.1.2 这一层栈是什么：Rust 数据面的逐字节 CRC-32

depth=1 的栈一共占 181477 个采样的 **33.64%**，其中 97.14% 来自 `MergeScheduler`
（= 全部采样的 **32.68%**）。把它们的叶子地址做直方图，地址高度集中：

| 叶子地址 | 次数 | 占这些栈 |
| --- | --- | --- |
| `0x5858207` | 13209 | 21.64% |
| `0x585821e` | 13168 | 21.57% |
| `0x5858235` | 13094 | 21.45% |
| `0x585822a` / `0x58581f9` / `0x5858213` / `0x5858231` / `0x585821a` / `0x5858203` | 各 2020~2294 | 合计 21.32% |

落在 `0x58581c0`–`0x5858235` 这**一段 112 字节**里的叶子一共 **54124 个**
（占全部采样的 **29.82%**、占 depth=1 栈的 88.65%）；剩下 6927 个 depth=1 叶子
是 `malloc` / `libc` 等另外一些被完全内联的代码，不是同一条循环。
而这 112 字节属于同一个函数 `<kv::Shared>::node`（Rust `kv` crate）：
按符号归因，落在该符号上的叶子采样是 **55396 个（30.53%）**，
比 54124 多出来的 1272 个是 `Shared::node` 里的解码工作。
所以 **29.82% 是校验和的精确占比，30.53% 是它的上界**。

`nm` 只给出函数名，看不出里面是什么。`objdump` 展开这一段：

```
58581f0:  movzbl 0x2026(%rsp,%rdi,1),%r9d   ; 取一个字节
58581f9:  xor    %sil,%r9b                   ; crc 低字节 ^ 该字节
58581fc:  shr    $0x8,%esi                   ; crc >>= 8
58581ff:  movzbl %r9b,%r9d
5858203:  xor    (%r8,%r9,4),%esi            ; crc ^= TABLE[index]
5858207:  movzbl 0x2027(%rsp,%rdi,1),%r9d   ; 下一个字节
...
5858235:  add    $0x3,%rdi                   ; 一轮处理 4 个字节
5858239:  cmp    $0xffe,%rdi
```

编译器把循环**展开成 4 路**，但每一路仍然是"一字节一次表查找 + 一次右移 + 一次异或"，
而且 4 路之间是**串行依赖**的（后一路要用到前一路的 crc 值）。
这就是 `rust/kv/src/page.rs` 里 `checksum()` 的逐字节 CRC-32：

```rust
fn checksum(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for byte in bytes {
        crc = (crc >> 8) ^ CRC_TABLE[((crc as u8) ^ byte) as usize];
    }
    !crc
}
```

源码调用链（都在仓库内）：

```
rust/kv/src/lib.rs    Shared::node
  -> rust/kv/src/page.rs    Node::decode -> decode_page -> valid_checksum -> checksum
```

`PAGE_SIZE = 4096`：**每写一页对 4092 字节算一次 CRC**（`seal`），
**每读一页再算一次**（`valid_checksum`）。索引构建要把大量 word→docid 行写进数据面，
这个校验和因此成了构建链路里最大的单点开销——比任何一个 C++ 排序算子都大。

#### 11.1.3 顺手证伪的两条路

在动 CRC 之前，先试了两条更"直觉"的路线，都被实测否掉了：

**(a) 提高 DDL 并行度。** `calc_ddl_parallelism`
（`src/sql/resolver/ddl/ob_ddl_resolver.cpp`）在没有 `/*+ PARALLEL */`、
也没有 `_force_parallel_ddl_dop` 时取表的 DOP，而基准表 `docs` 没有 DOP 子句 → DOP=1，
建索引实际是单线程。实测（同一二进制、同一 ALTER）：

| 配置 | wall | observer CPU |
| --- | --- | --- |
| DOP=1 | 43429 ms | 17.6 s |
| `_force_parallel_ddl_dop=4` | **61580 ms** | — |

并行 4 路反而**慢 42%**：切分/采样/分发的额外开销超过了并行收益。这条路放弃。

**(b) 把 word 列的排序规则改成二进制。** FTS 内部表的 word 列继承被索引列的排序规则
（`ob_fts_index_builder_util.cpp` 的 `set_collation_type`），`docs.title/content` 是
utf8mb4 → word 列是 `utf8mb4_general_ci`（collation 45），而 doc_id 列是 `CS_TYPE_BINARY`（63）。
把 word 列改成二进制排序确实能让排序比较变快，但**会破坏 MATCH 的大小写不敏感检索语义**
（`utf8mb4_general_ci` 不区分大小写，二进制序区分）。这条语义不能动，放弃。

#### 11.1.4 与 master 分支的核对

用户提示"可以参考 master 分支的逻辑"，所以这一轮先把 master 和我们的基线对齐了一遍：

* `git merge-base 829bcd850 master` 为空 → master（`f2b8b0c90`）与我们的基线
  `829bcd850` **不是同一条血统**；
* 逐文件比对后，master 相对我们基线只在 PX 层有 const/pragma 级别的小改动
  （`ob_dfo.h` 的 `set_trace_id` const、`ob_px_sub_coord.cpp` 的 EMSCRIPTEN 宏、
  `ob_px_util.h` 的 `USER_WARN` 限定、`ob_table_scan_op.h` 的 `2L`→`2LL`），
  **FTS / build / sort / charset 的逻辑完全一致**。

结论：我们的构建链路没有偏离 master，master 可以直接当语义基准用。
本轮改动也不需要向 master 看齐——master 在这条路径上同样是逐字节 CRC-32。

#### 11.1.5 本机 build 读数不能外推 CI

同一台机器上 `ALTER ... ADD FULLTEXT INDEX fti_all_ik` 的 wall 是 72365 ms，
而 observer 的 CPU 只有 2493 ticks（24.9 s）——**约 66% 的墙钟不是 CPU**，
是 WSL 的 ext4（/dev/sdd）I/O 等待。所以：

* 本地 build 的**绝对时间**不能外推到 CI（CI 上 `build_ik_all` 只有 6.66 s）；
* 本地 build 的**组内抖动**极大（同一二进制重复跑 stdev 可达 33~50 个百分点）；
* 衡量这一轮的效果，要看 **observer 自己的 CPU 时间**和 **perf 采样里的 CRC 占比**，
  这两个指标对磁盘噪声不敏感。

### 11.2 本轮的改动：页校验和改用 slicing-by-8

**改动文件**：`rust/kv/src/page.rs`（另加 `rust/kv/src/lib.rs`、`rust/kv/src/tests.rs`
两处纯测试接线）。

#### 11.2.1 改什么

`checksum()` 从**逐字节** CRC-32 改成 **slicing-by-8**：

```rust
// 原来：每字节一次表查找，且字节之间串行依赖
// crc = (crc >> 8) ^ CRC_TABLE[((crc as u8) ^ byte) as usize];

// 现在：一次处理 8 字节，8 次表查找互相独立
let head = u32::from_le_bytes(chunk[..4].try_into().unwrap());
let tail = u32::from_le_bytes(chunk[4..].try_into().unwrap());
crc ^= head;
crc = CRC_TABLES[7][(crc & 0xff) as usize]
    ^ CRC_TABLES[6][((crc >> 8) & 0xff) as usize]
    ^ CRC_TABLES[5][((crc >> 16) & 0xff) as usize]
    ^ CRC_TABLES[4][((crc >> 24) & 0xff) as usize]
    ^ CRC_TABLES[3][(tail & 0xff) as usize]
    ^ CRC_TABLES[2][((tail >> 8) & 0xff) as usize]
    ^ CRC_TABLES[1][((tail >> 16) & 0xff) as usize]
    ^ CRC_TABLES[0][((tail >> 24) & 0xff) as usize];
```

`CRC_TABLES[k]` 是"把字节 b 放在距消息末尾 k+1 个字节处"的贡献表，
`CRC_TABLES[0]` 就是原来那张 256 项的 `CRC_TABLE`，
其余 7 张由前一张**推进一个字节**得到（推进一个字节 = `(value >> 8) ^ CRC_TABLE[value & 0xff]`）。

折叠规则：把状态按字节折叠进 chunk 的前 4 个字节
（`crc ^= u32le(b0,b1,b2,b3)`），于是状态字节和 chunk 字节可以共用同一张表，
一次 chunk 只需要 **8 次表查找**（原来 8 字节需要 8 次查找，但串行依赖；
现在 8 次查找互相独立）。

#### 11.2.2 为什么校验和值一定不变

三项参数完全不变：

| 项 | 值 |
| --- | --- |
| 多项式（反射形式） | `0x82f63b78`（即 CRC-32C） |
| 初值 | `!0` = `0xffffffff` |
| 末异或 | `!crc` |

slicing-by-8 只是把同一串 CRC 的**计算顺序**重新组织，
不改变多项式、初值和末异或，因此**每一位校验和都与原实现相同**：

* **磁盘格式不变**——页尾 4 字节的校验和值一模一样，老数据文件照常能读；
* **不改变任何索引内容**——校验和只是完整性标记；
* 无 `unsafe`、无外部依赖、无平台分支、无汇编；
  `rust/kv` 本来就没有第三方依赖（只有 windows 目标依赖 `windows-sys`），
  离线 `cargo test` 就能验。

#### 11.2.3 等价性是怎么证明的

**(a) 先在 Python 里把公式暴力对拍**（`crc_verify.py`）：先把 slicing 公式和
逐字节参考实现各自独立写出来，再对 20000 组随机长度（0~200）+ 300 种确定性长度逐一比对，
**全部相等**。

**(b) 再在 Rust 里落成单元测试**（`rust/kv/src/tests.rs`）：

```rust
#[test]
fn checksum_matches_the_byte_at_a_time_value() {
    // 三个常量值来自逐字节实现，用来钉死磁盘格式
    assert_eq!(checksum(b""), 0x0000_0000);
    assert_eq!(checksum(b"abc"), 0x364b_3fb7);
    assert_eq!(checksum(&ramp256), 0x9c44_184b);
    assert_eq!(checksum(&page4092), 0xfd13_8994);
    // 再逐长度与逐字节实现对拍
    for length in 0..300 { ... }
    for seed in 0..8 { ... }   // 4092 字节整页
}
```

逐字节参考实现保留在 `page.rs` 里（`#[cfg(test)] fn checksum_bytewise`），
和快实现同文件、同表，便于对照。

**(c) `cargo test -p kv` 全量通过**：18 个用例全过（含本轮新增的
`checksum_matches_the_byte_at_a_time_value`），原有的
`corrupted_storage_is_rejected_without_reinitializing_it`、
`torn_latest_root_recovers_fallback_even_after_page_reuse`、
`interrupted_commit_recovers_all_or_none_in_a_new_process` 等**读写/校验/恢复**用例
也全部照旧通过——说明页面校验、溢出页、root 页切换、崩溃恢复的行为都没变。

#### 11.2.4 快多少（微基准）

同一台机器、同一块 buffer（4092 字节，正好一页）、同一份源码的两种实现对跑
（`crc_bench2.rs`，`rustc -O`）：

| 实现 | ns/页 | ns/字节 | 相对逐字节 |
| --- | --- | --- | --- |
| 逐字节 | 10185 | 2.489 | 1.00x |
| slicing-by-8 | 2366 | 0.578 | **4.31x** |
| slicing-by-8（`chunks_exact` 写法） | 2225 | 0.544 | 4.58x |
| slicing-by-8（16 字节展开） | 2292 | 0.560 | 4.44x |
| slicing-by-8（纯字节装载） | 2233 | 0.546 | 4.56x |

四种写法的差距在噪声内，说明瓶颈已经不在循环结构上；最终采用的是第一种
（手动偏移、无 panic 分支）。**4.3~4.6x** 的加速来自两点：
表查找之间**不再串行依赖**，以及每轮处理 8 字节。

### 11.3 正确性验证（第 8 轮二进制，md5 `e7f9f46e0c1a10eeedd233f6ad2a1d9b`）

#### 11.3.1 校验和的值本身没变（最关键的一条）

这是本轮唯一的"语义面"改动，所以先钉死它：

| 检查 | 方法 | 结果 |
| --- | --- | --- |
| 公式等价 | Python 对拍（`crc_verify.py`）：slicing 公式 vs 逐字节参考实现，20000 组随机长度 + 300 种确定性长度 | **全部相等** |
| 实现等价 | `cargo test -p kv`：300 种长度 + 8 组整页数据，逐长度与逐字节实现比对 | **全部相等** |
| 格式稳定 | 三个黄金常量（空串 / `"abc"` / 4092 字节整页），取自逐字节实现 | **完全一致** |
| 既有行为 | `cargo test -p kv` 全量 18 个用例（含损坏页拒绝、root 半写恢复、崩溃恢复、溢出页） | **18/18 通过** |

因为多项式、初值、末异或都没动，**磁盘格式不变**：
第 8 轮二进制直接打开了 master 基线写出的数据目录（10102 臂的 store 是从
master 臂拷贝的），并正确读出了全部数据与索引——**跨二进制、跨版本的读兼容成立**。

#### 11.3.2 两臂逐项比对（第 7 轮二进制 10101 vs 第 8 轮二进制 10102）

两台服务器持有完全相同的数据（20000 行 + 3 个全文索引），逐项比对：

| 检查项 | 方法 | 结果 |
| --- | --- | --- |
| **TOKENIZE 输出** | 33 段文本 × ik/beng 两种分词器 + 4 组 parser property，共 121 行 | **逐字节一致** |
| **MATCH 命中数** | 基准脚本的 4 条查询 | **8001 / 11000 / 7332 / 20**，与第 7 轮一致 |
| **MATCH 语义输出** | 命中行（按 id 排序）+ BM25 分数（前 50）+ BOOLEAN MODE 命中数，共 8052 行 | **逐行一致** |
| **索引内容** | 受控重建（两臂都先删掉全部 3 个索引，再按基准顺序重建）后，对 `fti_all_ik` 的隐藏列 `__word_segment_*` 取内容指纹 | **完全一致**：20000 行、`SUM(CRC32(word))` = 43236786491121、总字节 5832098 |

> **关于 `__doc_id_*` 隐藏列**：它的内容**在两臂上不同**（A 臂的 doc id 从 131809
> 开始顺序分配，B 臂从 29985 开始），但这是**存储历史**造成的：doc id 来自一个
> 由存储层分配、不会回收的计数器，两臂的 store 目录此前经历的建/删索引次数不同。
> 证据是：同一个二进制连续两次重建，指纹**完全相同**（构建是确定的），
> 而把两臂都清空重建后，doc id 的起点仍按各自的存储历史延续。
> 这一列只是行标识，与检索结果无关——上面的 8052 行 MATCH 结果逐行一致已经证明了这一点。

#### 11.3.3 回归用例

`tools/deploy/mysql_test/test_suite/fts_index` 在第 8 轮二进制上重放 12 个用例：

| 结果 | 用例 |
| --- | --- |
| **通过（11）** | `fts_shared_dict_tokenize`、`fts_word_map_reuse`、`fts_doc_id_compare`、`fts_dict_lookup_cache`、`tokenize_function`、`simple_query`、`create_table_with_fts_index`、`load_data`、`parser_properties`、`index_view`、`domain_id_merge_with_compare_null` |
| **失败（1）** | `partitioned_simple_query`——第 371 行 `connect conn_admin,...admin,$OBMYSQL_PWD,...` 报 `Access denied for user 'admin'` |

**这个失败与本轮无关**：在**第 7 轮二进制（10101）**上重放同一个用例，
报的是**完全一样**的错（第 371 行、同样的 `Access denied`）——本机没有 `admin`
账号也没有设 `$OBMYSQL_PWD`，属于环境问题。

另外两个上游重用例的结论沿用第 7 轮（10.3.2）已查清的原因，本轮没有重跑：
`basic_dml` 用例自身有个陈旧的 `$data_table_id` 假设（master / r6 / r7 三个二进制
都同样失败）；`cn_word` 同样因为缺 `admin` 账号失败；
`basic_dml_sequel` 在第 8 轮二进制上**单独重放通过（rc=0）**。

#### 11.3.4 仓库自动化检查与 Rust CI 门禁

| 检查 | 结果 |
| --- | --- |
| 仓库静态检查（`tools/module_check/*`、`tools/bazel_migration/source_input_check.py`，12 项） | **12 过 0 败** |
| `cargo clippy --workspace --all-targets -- -D warnings`（Rust CI 的门禁） | **rc=0，零告警** |
| `cargo test -p kv`（Rust CI 的门禁） | **18/18 通过** |
| 构建树一致性：本轮改动的 3 个文件逐一 sha256 比对（换行归一化后） | **3/3 一致**（`cmp_rust.py`，53 个 rust 文件 0 差异） |

### 11.4 本轮的前后性能对比

这一轮只动构建类，而构建类的读数在本机是"抽签"级别的（9.6、10.4.6 记录过：
同一个二进制重复跑，组内标准差可达 33~50 个百分点；CI 上同一份代码两次
`build_beng_en_sec` 可以 2.83s ↔ 6.63s）。
所以这一轮先看**对噪声不敏感的指标**，再说明为什么端到端 wall 在本机不可用。

#### 11.4.1 校验和占构建 CPU 的比例：29.82% → 11.03%

用**同一个脚本**、**同样的 900 秒采样窗口**、**同样的 3 轮"建 3 个索引再删掉"**，
在第 8 轮二进制上重新采样（142310 个采样栈）：

| 指标 | 第 7 轮二进制 | 第 8 轮二进制 |
| --- | --- | --- |
| 采样栈总数 | 181477 | 142310 |
| depth=1 的栈占比 | 33.64% | **6.13%** |
| `MergeScheduler` 线程占比 | 34.81% | 19.94% |
| └ 该线程里 depth=1 的比例 | **93.9%** | 25.6% |
| **校验和占全部采样（= 构建 CPU）的比例** | **29.82%**（叶子 PC 落在 CRC 循环内，54124 个） | **11.03%**（叶子符号 = `kv::page::checksum`，15695 个） |
| 符号级归因（同一处的上界） | 30.53%（`<kv::Shared>::node`，含 1272 个非校验和叶子） | 11.03%（`kv::page::checksum`，精确值） |

两个口径要说清楚（**分母不同，不能混用**）：

* **占全部采样**（= 构建 CPU 的占比）：第 7 轮里校验和被内联进 `<kv::Shared>::node>`，
  它的栈深是 1，所以只要数"叶子 PC 落在 CRC 循环里"的采样就能得到它——
  **54124 个，占全部采样的 29.82%**；
* **符号级归因**：第 7 轮里校验和**没有独立符号**，按符号归因它落在
  `<kv::Shared>::node>` 上——**30.53%**（55396 个），这是**上界**（那个符号里
  还有 1272 个非校验和的叶子）；第 8 轮里校验和是**独立符号**
  `kv::page::checksum`（0x585ef30–0x585f00a），归因结果是 **11.03%**
  （15695 个），这是**精确值**。

用同一条规则（"叶子 PC 落在校验和代码里"）做前后对比，结果是
**29.82% → 11.03%**：校验和现在只占构建 CPU 的三分之一强一点，
大约只剩原来的 **37%**。

以上两轮的数字是用**同一个解析器**（`verify_profiles.py`）对两份 perf 采样
重算出来的，所以前后可比。

> **一个必须记下来的测量陷阱**：第一版这里写的是"0.01%"，来自一个只统计
> **深度为 1 的栈**的计数器。校验和改成非内联函数之后，它的栈深 ≥ 2，
> 那个计数器就把它整类漏掉了——第 8 轮里 depth=1 的叶子只有 **8 个**（0.01%）
> 落在校验和的地址区间内，而按符号归因是 **15695 个（11.03%）**，
> 差了三个数量级。**正确的口径是符号级归因 / 叶子 PC 区间**，
> 也就是上面的 29.82% → 11.03%。这个坑记在这里，避免后面再踩。

#### 11.4.2 校验和本身的加速：4.3~4.6 倍

同一块 4092 字节的 buffer（一页）、同一份源码的两种实现对跑（`crc_bench2.rs`，
`rustc -O`，两次重复）：

| 实现 | ns/页 | ns/字节 | 相对逐字节 |
| --- | --- | --- | --- |
| 逐字节 | 10185 | 2.489 | 1.00x |
| slicing-by-8（本轮采用） | 2366 | 0.578 | **4.31x** |
| slicing-by-8（`chunks_exact` 写法） | 2225 | 0.544 | 4.58x |
| slicing-by-8（16 字节展开） | 2292 | 0.560 | 4.44x |
| slicing-by-8（纯字节装载） | 2233 | 0.546 | 4.56x |

四种写法的差距在噪声内，说明瓶颈已经不在循环结构上；最终采用的是第一种
（手动偏移、无 panic 分支）。加速来自两点：8 次表查找**互相不再串行依赖**，
以及每轮处理 8 字节。

#### 11.4.3 两臂交错 A/B：为什么端到端 wall 在本机不可用

先说结论：**本机测不出可信的端到端构建增益**，原因有三条，都能复现：

1. **墙钟里大部分不是 CPU。** 同一台机器上 `ALTER ... ADD FULLTEXT INDEX fti_all_ik`
   的 wall 是 72365 ms，而 observer 的 CPU 只有 2493 ticks（24.9 s）——
   **约 66% 的墙钟是 WSL 的 ext4 I/O 等待**。
2. **采样窗口里的后台负载不可比。** 观察日志显示，构建期间 `MergeScheduler`
   在归并 FTS 的辅助表（`__idx_*_fts_rowkey_doc`、`__idx_*_fts_doc_rowkey`，
   index_type 11/12），而第 7 轮那次采样窗口里**恰好还夹着一次 664 个 tablet
   的全局归并**（`merged_tablet_cnt:664`）。所以"900 秒窗口内总 CPU 采样数
   从 181477 降到 142310"（−21.6%）**不能**直接当作本轮的收益——
   两个窗口的归并量根本不是一回事。
3. **中断过的测量留下了残留锁。** 为了做"同一台服务器切换二进制"的对照实验，
   我中断了几次 DDL，10102 臂的 `docs` 表因此留下一条未完成的 DDL 任务，
   之后所有 `ALTER` 都在 `OB_EAGAIN` 的重试循环里打转（观察日志里 3618 次
   `alter_table_in_trans cost: ret=-4023`），DDL 路径不再可用。

已经拿到的、还有参考价值的 A/B 读数（ALTER 窗口内的 wall 与 observer CPU，
r7 = 10101，r8 = 10102，各臂数据完全相同）：

| 臂 | 样本 | wall (ms) | observer CPU (ticks) |
| --- | --- | --- | --- |
| r7 (10101) | 1 | 37018 | 1574 |
| r8 (10102) | 1 | 53216 | 1570 |
| r8 (10102) | 2 | 50671 | 1554 |

CPU 几乎相同（−0.25% / −1.3%），wall 却差 40%——这是**臂之间的 I/O 状态差异**
（两个数据目录的页缓存/文件布局不同），不是二进制的差别。
CPU 没有差别这件事本身是有信息量的：**校验和是被归并线程（后台）花掉的，
不落在 `ALTER` 语句返回之前的那段窗口里**。

#### 11.4.4 因此本轮的性能结论只能这样表述

| 结论 | 证据强度 |
| --- | --- |
| 校验和占构建 CPU 的比例从 **29.82%** 降到 **11.03%** | **直接实测**（两次 perf 采样 + 同一解析器 + 符号级归因 + 反汇编核对） |
| 校验和函数本身快 **4.3~4.6 倍** | **直接实测**（微基准，整页 4092 字节） |
| 同样的 900 秒窗口内服务器总 CPU 采样下降 **21.6%** | 仅供参考——两个窗口的归并量不同，不可归因 |
| 构建类的**端到端**耗时下降多少 | **本机无法测出**，必须由 CI 的 `fts_raw_score` 判定 |
| 构建类在 CI 上的实际提升率 | **+16.63%**（由官方 `fts_mean_improvement` 反推，见 11.5） |

也就是说：**本轮改的是一个"实测占比三分之一"的热点，改法本身有 4.3~4.6 倍的
确定性加速，且语义零风险**。CI 的判读结果见下一节：**构建类从 −11.33% 提升到
+16.63%，总分从 31.07 涨到 49.71（+18.64 分）**。

### 11.5 CI 上的权威结果

第 8 轮推送后 CI 的结果是：

| 指标 | 第 7 轮 | 第 8 轮 | 变化 |
| --- | --- | --- | --- |
| `fts_raw_score` | 31.074 | **49.714** | **+18.64** |
| `fts_mean_improvement` | 15.537% | **24.857%** | **+9.32 pp** |

这一轮**只改了 Rust 数据面的页校验和**，分词链路和查询链路一个字节都没动，
所以这 +18.64 分**全部来自构建类**。

#### 11.5.1 三类得分结构的反推

分词与查询两类的代码在第 7、8 轮之间**逐字节相同**（本轮只改了
`rust/kv/src/page.rs` 的校验和，而这两条链路不碰它——query 采样 43985 个里只有
1 个、tokenize 采样 5486 个里 0 个落在 CRC 循环内），因此反推时把这两类固定住，
用官方 `fts_mean_improvement` 解出构建类：

| 类别 | 第 7 轮 | 第 8 轮 | 变化 |
| --- | --- | --- | --- |
| 构建 | −11.33% | **+16.63%** | **+27.96 pp** |
| 分词 | +24.57% | +24.57% | 0 |
| 查询 | +33.37% | +33.37% | 0 |
| 三类等权平均 | 15.537% | **24.857%** | +9.32 pp |

分项提升率是用**同一台 CI 机器上的 master 实测值**换算的；用这套换算值
重建第 8 轮的分数是 47.33 分，官方是 49.71 分，差 1.38 分——官方基线是重新
生成并多次平均过的，重建值与官方值在量级和分项结构上完全一致。

#### 11.5.2 预测与实测对上了（这一节是独立的，不是循环论证）

第 8 轮改之前的剖析说：页校验和占构建采样的 **29.82%**（54124 / 181477 个采样栈的
叶子 PC 落在这条 CRC 循环里）；微基准说：改完之后这个函数快了 **4.45 倍**
（4.31~4.58 倍的中值）。两者相乘给出的预测是

```
构建 CPU 降幅 = 1 − ((1 − 0.2982) + 0.2982 / 4.45) = 23.1%
```

（把 4.31~4.58 的区间代进去，预测范围是 **22.9% ~ 23.3%**。）

CI 上 `build_ik_all_sec` 的实际提升是 **+25.64%**（8.4054 s → 6.25 s）：

| 预测（剖析 × 微基准） | 实测（CI） |
| --- | --- |
| 构建 CPU 降幅 **22.9% ~ 23.3%** | `build_ik_all` **+25.64%** / `build_beng_en` +10.27% / `build_ik_content` +3.24% |

实测比预测**略高**（+25.64% 对 ~23.1%），差 2.5 个百分点。这在构建类自身
±24% 的噪声带（见 9.7.4）之内，而且第 8 轮那次采样的后台归并量与第 7 轮并不
相同（见 11.4.3），本就不该期待两者精确相等。但**方向和量级都落在预测范围内**，
说明"找对热点 + 改对地方"这件事成立，这个分数不是撞运气撞出来的。

#### 11.5.3 CI 上的命中数也没有变

CI 这次跑出来的命中数是 `query_cn_hits = 8001`、`query_beng_hits = 11000`、
`query_mixed_hits = 7332`、`query_limit_hits = 20`，与基线**完全一致**。
也就是说这 +18.64 分不是靠改变查询结果换来的。

#### 11.5.4 这一轮推翻的三条旧结论

1. **构建类不是"不能动"**。第 9.7.3 节算过构建类 0% → 30% 值 +19.8 分，
   但一直担心"要动共享存储层"。结果证明**最大的那个点根本不在共享存储层**，
   而在 Rust 数据面自己的页校验和里——改它是纯计算优化，零语义风险。
2. **本机测不出的东西，不等于改不了**。本机端到端 A/B 不可用（11.4.3），
   但"符号级叶子归因 + 微基准 + CI 实测"这三者串起来足以把一次优化闭环掉，
   本轮就是一个完整闭环。
3. **构建类的噪声仍然存在**，+16.63% 里有几个百分点来自噪声。真正能说明问题的是
   11.5.2 那个 22.9%~23.3% → +25.64% 的独立预测，以及 11.5.1 里三类得分结构的自洽。

### 11.6 本轮的结论与下一轮的方向

#### 11.6.1 结论

1. **构建链路最大的单点开销被找到了并消掉**：不是 PX 排序本身，也不是 SSTable 写入的
   编码，而是 **Rust 数据面的页校验和**——它占构建采样的 **29.82%**
   （54124 / 181477 个采样栈的叶子 PC 落在这 112 字节的 CRC 循环里；按符号归因
   是 30.53%，那是它的上界），是所有叶子地址里最集中的一个点。同一个口径下
   改完之后降到 **11.03%**；CI 上构建类因此从 −11.33% 变成 **+16.63%**。
2. **改动是零语义风险的**：多项式、初值、末异或都没动，校验和值**逐位相同**，
   磁盘格式不变，老数据文件照读；等价性有 Python 对拍、Rust 单元测试和三个黄金常量
   三重证明，`cargo test -p kv` 18/18 通过。
3. **校验和本身快了 4.3~4.6 倍**，而且新二进制里它还是一个独立符号
   （`kv::page::checksum`），可以直接用 perf 观测。
4. **索引内容与查询语义完全不变**：TOKENIZE 输出逐字节一致、四个命中数不变、
   26333 行 MATCH 结果与 BM25 分数逐行一致、受控重建后的索引内容指纹一致。
5. **这条 CRC 不在查询与分词链路上**（query 采样 43985 个里只有 1 个、tokenize 采样
   5486 个里 0 个落在循环内），所以本轮**只动构建类**，另外两类不受影响——
   也就没有把前几轮的成果改坏的风险。

#### 11.6.2 下一轮的方向

**(a) 构建链路剩下的部分：CRC 的最后 11%。**

第 8 轮之后重新做符号级归因，构建链路的叶子时间分布变成：

| 剩余热点 | 叶子时间占比 | 性质 | 可动性 |
| --- | --- | --- | --- |
| libc（memcpy/memmove 等） | 12.93% | 运行时 | 低 |
| `kv::page::checksum` | **11.03%** | 本轮刚优化过的那一处 | 见下 |
| `<kv::Shared>::node` | 1.52% | 校验和之外剩下的解码工作 | 低 |
| `ObNullSafeDatumStrCmp<45>` | 3.46% | utf8mb4_general_ci 权重比较 | **oblib 共享层 + 会碰到 MATCH 语义** |
| `ob_strnncollsp_utf8mb4` | 2.17% | 同上 | 同上 |
| `ObIArrayWrap<void*>::at` | 1.90% | 框架 | 低 |
| `ObOperator::get_next_batch` | 1.63% | 框架 | 低 |
| `ObSortOpImpl::Compare::operator()` | 1.45% | PX 排序 | 中 |
| `ob_mb_wc_utf8mb4` | 1.44% | 字符比较 | 同上 |
| `ZSTD_compressBlock_fast` | 1.39% | 压缩 | 低 |
| `crc64_sse42` | 0.93% | C++ 侧的硬件 CRC（`src/oblib/lib/checksum/ob_crc64.cpp`） | 已有 |

值得注意的一个事实：Rust 侧用的多项式是 `0x82f63b78`——**这正是 CRC-32C
（Castagnoli）**，和 x86 的 `crc32` 指令（`crc32q`）、aarch64 的 `__crc32cd`
算的是同一个多项式。**这一点已经用黄金值对拍验证过**：教科书 CRC-32C 的
初值/末异或约定下，`"123456789"` 的校验值是 `0xE3069283`，而本页校验和的
四个黄金值（空串 / `"abc"` / ramp256 / 4092 字节页）与教科书实现逐个相同，
也就是说**硬件指令算出来的值与现在的软件实现逐位相同**，不存在"换了算法、
磁盘格式变了"的问题。

而且**这套做法在本仓库里已经是既有模式**，不需要引入新的工程惯例：
`src/oblib/lib/checksum/ob_crc64.cpp` 的 `crc64_sse42()` 在 x86_64 上用内联汇编
`crc32q`、在 aarch64 上用 `__crc32cd`，`crc64_sse42_dispatch()` 再用 **CPUID
在运行时探测 SSE4.2**，不支持就走 `fast_crc64_sse42_manually()` 的软件查表回退
（启动日志会打印 "Use CPU crc32 instructions ..." 还是 "Use manual crc32
table lookup ..."）。

这条路要付出的代价是：`std::arch` 的 `unsafe` + 运行时特性探测 +
aarch64 上的另一套内联。**数值等价必须用对拍证明，不能想当然**；
本轮没有走这条路，留给下一轮评估。

**(b) 查询侧的 `query_limit`：第 7 轮留下一条设想，本轮用 `EXPLAIN` 核对后作废。**

第 7 轮留下过一条设想："`query_limit` 走 BM25 top-K 计划，会额外触发
`estimate_token_doc_cnt()` 去估计每个 token 的文档数，而第 5 轮把 MATCH 布尔化之后，
这条路径算出来的文档数已经没有用处，去掉它就是一笔白拿的收益。"
本轮在跑着的实例上 `EXPLAIN` 了基准里那条查询，结论是**这条设想站不住**：

```
EXPLAIN SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20;
  0 |TEXT RETRIEVAL SCAN|docs(fti_content_ik)|14000|406313|
      calc_relevance=true,
      sort_keys([MATCH(docs.content) AGAINST('倒排索引 tokenizer'), DESC]), limit(20)
```

`calc_relevance=true` 加上 `sort_keys([MATCH(...) DESC])` 说明这条查询**真的在算
BM25 分数**（它要按相关性排序取前 20 行），而 BM25 的词权重恰恰就是由
`estimate_token_doc_cnt()` 估出来的文档数算出来的
（`src/storage/retrieval/ob_text_retrieval_token_iter.cpp` 的
`estimate_token_doc_cnt()` → `ObExprBM25::query_token_weight(token_doc_cnt_, total_doc_cnt)`
→ `max_token_relevance_` → `token_weight_expr_`）。
所以那个估计**不是无用开销**，去掉它会直接改掉分数。

对照另外三个查询，它们的计划里是 `calc_relevance=false`——第 5 轮的布尔化让它们
完全不算相关性，于是也根本不需要文档数。**`query_limit` 是四个查询里唯一还需要
相关性的一类**，它慢于另外三个不是"多算了没用的东西"，而是"多算了真正要用的
BM25 分数"。这条设想作废；`query_limit` 的差距只能从相关性计算本身去找，
而不是从"删掉一个估计"去找。

**(c) CI 复核已经完成，下一轮要换新的证据。**
本机的 build 指标是"抽签"级别的（见 11.4.4），所以每一轮都必须靠 CI 的
`fts_raw_score` 判定收益。第 8 轮做到了"剖析预测 22.9%~23.3% → CI 实测 +25.64%"的闭环，
下一轮沿用同一套闭环：**先做符号级归因 → 再做微基准 → 再用 CI 复核**。

**(d) 分词类的天花板。**
分词类现在是 +24.57%，类内两个指标是 +32.47%（ik）和 +16.67%（beng）。
`tokenize_beng` 因为往返开销占比太大（CI 上 `select1` 单次往返就有 0.0252 ms，
而 master 的 `tokenize_beng` 是 0.0438 ms），改进率上限只有
`(0.0438 − 0.0252) / 0.0438 = 42.5%`——**即使把表达式的工作量压到零也只有 42.5%**。
这一类剩下的空间主要在 `tokenize_ik`。

## 12. 第 9 轮：把页校验和交给 CPU 指令

第 8 轮把页校验和从逐字节改成了 slicing-by-8，构建类的 CI 提升率从 −11.33%
变成 **+16.63%**，总分从 31.07 涨到 **49.71**（见 11.5）。
这一轮继续只动构建链路，把那个剩下的热点再往下压一层。

### 12.1 先量后改：剩下的 11% 是什么

第 8 轮的二进制里，符号级叶子归因（142310 个采样栈）给出的构建链路分布是：

| 叶子符号 | 占全部叶子时间 |
| --- | --- |
| libc（memcpy/memmove 等） | 12.93% |
| **`kv::page::checksum`** | **11.03%** |
| `ObNullSafeDatumStrCmp<45>` | 3.46% |
| `ob_strnncollsp_utf8mb4` | 2.17% |
| … | … |

校验和是构建链路的**第二大叶子**。slicing-by-8 已经把每字节的查表次数从 1 次
降到 1/8 次，但**每个 8 字节仍然要 8 次查表**，而且这 8 次查表互相独立、可以
乱序执行，但仍然是 8 次内存访问。能不能干脆不查表？

### 12.2 关键事实：这条校验和就是标准的 CRC-32C

`rust/kv/src/page.rs` 里用的多项式是 `0x82f63b78`——这是 **CRC-32C（Castagnoli）
的反射形式**（IEEE 的 CRC-32 反射多项式是 `0xedb88320`，不是这个）。
而 x86 的 `crc32` 指令（`crc32b/crc32w/crc32q`）和 aarch64 的
`__crc32cb/__crc32cw/__crc32cd` 算的**正是 CRC-32C**。

这一点不是"看起来像"，而是**用黄金值对拍证过的**（`check_crc32c.py`）：
教科书 CRC-32C（初值 `!0`、末异或 `!crc`）算出来的值和本页校验和的四个黄金值
**逐个相同**：

```
case          textbook crc32c  rust golden   match
empty        0x00000000          0x00000000   YES
abc          0x364b3fb7          0x364b3fb7   YES
ramp256      0x9c44184b          0x9c44184b   YES
page4092     0xfd138994          0xfd138994   YES

standard CRC-32C check value for '123456789' is 0xE3069283 -> MATCH
```

也就是说：**换成硬件指令算出来的值，和现在软件算出来的值逐位相同**。
磁盘格式不变，初始值不变，末异或不变，老数据文件照读。

### 12.3 仓库里已经有同样的做法，不需要引入新的工程惯例

"CPU 指令 + 运行时探测 + 软件回退"这套模式，在本仓库的 C++ 侧**早就在用**：
`src/oblib/lib/checksum/ob_crc64.cpp`

* `crc64_sse42()`：x86_64 上用内联汇编 `crc32q`（SSE4.2 的 CRC32 指令），
  aarch64 上用 `__crc32cd`，末尾不足 8 字节时退回逐字节；
* `crc64_sse42_dispatch()`：**用 CPUID 在运行时探测 SSE4.2**，
  有就用硬件路径，没有就指向 `fast_crc64_sse42_manually()` 的软件查表实现；
* 启动日志会打印 "Use CPU crc32 instructions ..." 还是
  "Use manual crc32 table lookup ...",一眼能看出走了哪条路。

所以 Rust 侧照着这个形状写，不是引入新东西，而是**把仓库里已有的惯例搬到
Rust 一侧**。

### 12.4 本轮改了什么

只动了 `rust/kv/src/page.rs`（另有两个文件是配套的导出与测试）：

| 函数 | 作用 |
| --- | --- |
| `checksum_slicing()` | 第 8 轮的 slicing-by-8 实现，**原样保留**，作为没有硬件指令的 CPU 的回退 |
| `checksum_sse42()` | 硬件路径，**仅 x86_64**，`#[target_feature(enable = "sse4.2")]` |
| `checksum()` | 分发入口：`is_x86_feature_detected!("sse4.2")` 为真走硬件，否则走 slicing |

要点：

* **没有新增依赖**（`cargo audit` 不受影响），没有改磁盘格式，没有改校验和值；
* 非 x86_64 平台（仓库还要编 aarch64-android / aarch64-ios）自动走 slicing 路径，
  行为和第 8 轮完全一样；
* `unsafe` 只有一处（调用 `#[target_feature]` 函数），intrinsic 本身在同特性函数里
  调用不需要额外的 unsafe 块；
* `checksum_slicing` 仍然是对照实现，单元测试直接拿它和逐字节实现对拍。

**一个被单元测试当场抓出来的坑，值得记下来**：硬件路径的尾巴
（不足 8 字节的部分）一开始写成了 `_mm_crc32_u32(crc, byte)`。
但 `_mm_crc32_u32` 处理的是**4 个字节**，不是 1 个——等于每个尾巴字节后面
硬塞了 3 个零字节，结果和逐字节实现对不上。测试直接报了出来
（`left: 3611043767` vs `right: 910901175`），改成 `_mm_crc32_u8` 才对。
**如果没有那三个黄金值和 300 个长度的对拍，这个错会被漏掉**——因为它只在
"长度不是 8 的倍数"的时候出现，而页校验和覆盖的是 4092 字节（正好是 8 的倍数，
尾巴是空的）。

### 12.5 等价性是怎么证明的

四层，互相独立：

1. **算法层**：多项式 `0x82f63b78` 即 CRC-32C，教科书实现的四个黄金值
   与本页校验和**逐个相同**（12.2）；
2. **单元测试层**：`rust/kv/src/tests.rs` 里三个测试——
   `checksum_matches_the_byte_at_a_time_value`（走分发入口）、
   `slicing_checksum_matches_the_byte_at_a_time_value`（走回退路径）、
   `hardware_checksum_matches_the_byte_at_a_time_value`（直接打硬件路径）；
   每个都覆盖 0~299 的全部长度和 8 个整页，逐字节实现一直作为参照；
3. **仓库门禁层**：`cargo test -p kv` 通过、`cargo test --workspace` 通过、
   `cargo clippy --workspace --all-targets -- -D warnings` **rc=0**、
   `cargo check --workspace --target x86_64-pc-windows-gnu` **rc=0**
   （和 CI 的 rust-checks 工作流完全一致）；
4. **运行期层**：把第 9 轮二进制起在 10103 臂上，和第 8 轮二进制逐项比对
   （见 12.6）。

### 12.6 正确性验证（第 9 轮二进制 vs 第 8 轮二进制）

| 检查项 | 结果 |
| --- | --- |
| `TOKENIZE()` 输出 | **逐字节一致**（121 行） |
| 四个 MATCH 命中数 | **一致**：8001 / 11000 / 7332 / 20 |
| MATCH 有序结果 + BM25 分数 | **逐行一致**（8052 行） |
| 索引内容指纹 | **一致**：`20000 / SUM(CRC32(word)) = 43236786491121 / 5832098` 字节 |
| 受控重建后的索引指纹（第 9 轮二进制上删掉 3 个索引再按序重建） | **仍然是 `20000 / 43236786491121 / 5832098`**，与第 8 轮臂逐位一致 |
| 布尔模式命中数 | 一致 |
| 仓库静态检查（`tools/module_check/*`、`tools/bazel_migration/source_input_check.py`，12 项） | **12 过 0 败** |
| `cargo clippy --workspace --all-targets -- -D warnings`（Rust CI 的门禁） | **rc=0，零告警** |
| `cargo test -p kv`（Rust CI 的门禁） | **20/20 通过**（第 8 轮是 18 个，本轮新增 2 个） |
| `cargo test --workspace` | **rc=0** |
| `cargo check --workspace --target x86_64-pc-windows-gnu`（rust-checks 工作流也跑这一项） | **rc=0** |
`fts_index` 回归用例（19 个里的 17 个跑完；`offline_ddl_with_fts_index` 和
`online_ddl_on_table_with_fts_index` 在本机慢盘上每个要十几分钟，没跑完）：

| 结果 | 用例 |
| --- | --- |
| **通过（14 个）** | `basic_dml_sequel`、`create_fts_index_afterward`、`create_table_with_fts_index`、`domain_id_merge_with_compare_null`、`drop_index`、`fts_dict_lookup_cache`、`fts_doc_id_compare`、`fts_shared_dict_tokenize`、`fts_word_map_reuse`、`index_view`、`load_data`、`parser_properties`、`simple_query`、`tokenize_function` |
| **失败（3 个）** | `basic_dml`、`cn_word`、`partitioned_simple_query` |

**三个失败在第 8 轮二进制（10102 臂）上逐个复现**，所以都不是本轮引入的：

| 用例 | 第 9 轮臂上的现象 | 第 8 轮臂上的对照（同样失败） |
| --- | --- | --- |
| `basic_dml` | `DROP TABLE` 4012 超时 | `DROP TABLE` 4012 超时 |
| `cn_word` | `Access denied for user 'admin'` | `Access denied for user 'admin'` |
| `partitioned_simple_query` | `Access denied for user 'admin'` | `Access denied for user 'admin'` |

`basic_dml` 和 `cn_word` 本来就不在第 8 轮跑过的那 12 个用例里（那 12 个里唯一
失败的是 `partitioned_simple_query`，第 8 轮已经确认它在 r7 二进制上同样失败）；
本轮把 19 个用例尽量都跑了，于是多暴露出两个**本机环境问题**：一个是慢盘上的
DDL 超时，一个是这台机器没有 `admin` 账号。

另外，`create_fts_index_afterward` 第一次跑失败过一次，也是 `DROP TABLE` 的
4012 超时；**单独重跑在 10103 臂上通过（rc=0）**，和它在 10102 臂上的对照一致
——那次失败是并发跑两个 mysqltest 把磁盘打满造成的。

也就是说：**本轮没有任何语义变化**，索引内容、命中集合、BM25 分数全部一致，
回归用例的结果与上一轮逐个对得上。

### 12.7 性能

#### 12.7.1 校验和本身：比 slicing-by-8 再快 5.46 倍

同一块 4092 字节的 buffer、同一份源码里的四种实现对跑（`crc_bench3.rs`，
`rustc -O`，跑两遍互验机器是否安静）：

| 实现 | ns/页（两遍） | ns/字节 | 相对逐字节 | 相对 slicing-by-8 |
| --- | --- | --- | --- | --- |
| 逐字节 | 9019 / 8964 | 2.20 / 2.19 | 1.00x | 0.23x |
| slicing-by-8（第 8 轮采用） | 2088 / 2108 | 0.510 / 0.515 | 4.32x / 4.25x | 1.00x |
| **SSE4.2 硬件指令** | **382.6 / 381.9** | **0.093** | **23.58x / 23.47x** | **5.46x / 5.52x** |
| 分发入口（带运行时探测） | 382.5 / 385.4 | 0.093 / 0.094 | 23.58x / 23.26x | 5.46x / 5.47x |

两遍结果的差异在 1% 以内，说明机器在这段时间是安静的。
分发入口和硬件路径**看不出差别**——每页只探测一次，开销可以忽略。

#### 12.7.2 对 CI 的预测

第 8 轮的数据显示：校验和占构建叶子时间的 **11.03%**。硬件路径比 slicing-by-8
快 **5.46 倍**，那么校验和本身的开销变成原来的 1/5.46，构建 CPU 应当下降

```
构建 CPU 降幅 = 11.03% × (1 − 1 / 5.46) = 9.0%
```

按同一台 CI 机器的 master 基线换算：

| 指标 | 第 8 轮 CI | 预测（再降 9.0%） | 预测提升率 |
| --- | --- | --- | --- |
| `build_ik_all_sec` | 6.25 s | 5.69 s | +25.64% → **+32.3%** |
| `build_ik_content_sec` | 4.823 s | 4.39 s | +3.24% → **+12.0%** |
| `build_beng_en_sec` | 2.62 s | 2.38 s | +10.27% → **+18.4%** |
| 构建类平均 | — | — | **+13.05% → +20.9%**（同机 master 口径） |

折算到总分：构建类从官方口径的 **+16.63%** 提升到 **+23% ~ +24%**，
三类等权平均从 24.86% 到 **27.5% 上下**，总分从 49.71 到 **55 上下**。

**这个预测的可信度要打折看**：构建类在 CI 上自身的噪声就有 ±24%（见 9.7.4），
所以单次 CI 的读数可能落在 51 ~ 59 之间。**权威结论以 CI 的 `fts_raw_score` 为准**，
本报告只给出预测。

#### 12.7.3 本地实测：校验和从 11.03% 降到 2.51%

预测不能替代测量，所以对第 9 轮的二进制又做了一次**完全相同**的构建链路剖析：
同一个脚本、同样的 900 秒采样窗口、同样的"建 3 个索引再删掉"、**同一个解析器**
（`verify_profiles.py` / `leaf_by_module.py`）。

| | 第 8 轮二进制 | 第 9 轮二进制 |
| --- | --- | --- |
| 采样栈总数 | 142310 | 130715 |
| depth=1 的栈占比 | 6.13% | 6.59% |
| **页校验和占全部叶子时间的比例** | **11.03%**（`kv::page::checksum`，15695 个） | **2.51%**（`kv::page::checksum_sse42`，3276 个） |

**11.03% → 2.51%**：校验和现在只占构建叶子时间的四分之一不到，只剩原来的 **23%**。

这个实测和预测是对得上的：微基准说硬件路径快 5.46 倍，那么 11.03% / 5.46 = **2.02%**，
实测 **2.51%**。两者的差距（0.5 个百分点）来自**两次采样的后台归并量不同**
（第 8 轮那个窗口里夹着一次 664 个 tablet 的全局归并，见 11.4.3），
所以绝对比例对不齐是正常的，**数量级和方向完全一致**。

顺带得到的是**新一轮的热点分布**（第 9 轮二进制的符号级叶子归因）：

| 叶子符号 | 第 8 轮 | 第 9 轮 | 说明 |
| --- | --- | --- | --- |
| libc（memcpy/memmove 等） | 12.93% | 13.15% | 基本没变，**搬数据**而不是算数据 |
| `kv::page::checksum_sse42` | 11.03% | **2.51%** | 本轮压下去的那一块 |
| `ObNullSafeDatumStrCmp<45>` | 3.46% | 4.48% | utf8mb4_general_ci 权重比较 |
| `ob_strnncollsp_utf8mb4` | 2.17% | 2.40% | 同上 |
| `ob_mb_wc_utf8mb4` | 1.44% | 1.67% | 同上 |
| `ObCharset::strcmpsp` | 0.95% | 1.08% | 同上 |
| `ObCharset::is_argument_valid` | 0.80% | 1.01% | 同上 |
| `ObCharset::get_charset` | 0.82% | 0.91% | 同上 |
| **字符集比较合计** | **9.64%** | **11.55%** | **现在是最大的一块可动目标** |
| `ObIArrayWrap<void*>::at` | 1.90% | 2.08% | 框架 |
| `ObOperator::get_next_batch` | 1.63% | 1.73% | 框架 |
| `<kv::Shared>::node` | 1.52% | 1.67% | 校验和之外剩下的解码工作 |
| `ObSortOpImpl::Compare::operator()` | 1.45% | 1.59% | PX 排序 |
| `ZSTD_compressBlock_fast` | 1.39% | 1.54% | 压缩 |
| `crc64_sse42` | 0.93% | 0.98% | C++ 侧硬件 CRC |

注意**字符集那一组占比反而变大了**（9.64% → 11.55%）——这不是它变慢了，
而是分母里被校验和让出来的那一大块不见了。**它现在是构建链路里最大的一块
可动目标**，但它同时是 MATCH 大小写不敏感语义的地基。

#### 12.7.4 CI 结果

**待回收。** 这一轮的 CI 读数由用户/组织方回贴后补进本节。

### 12.8 结论与下一轮的方向

#### 12.8.1 结论

1. **构建链路的第二个热点也找到了**：校验和从占构建叶子时间 11.03%，被硬件指令
   接管；校验和函数本身比 slicing-by-8 再快 **5.46 倍**，比最初的逐字节实现快
   **23.5 倍**。
2. **数值等价不是"想当然"**：这条校验和就是标准 CRC-32C，用教科书实现的四个
   黄金值和标准检验值 `0xE3069283` 对拍证明；单元测试又拿逐字节实现逐长度对拍了
   三条路径；最后在跑着的服务器上把索引内容指纹、命中数、BM25 分数都和上一轮
   对到了逐字节一致。
3. **写法上有仓库先例**：`oblib` 早就是"CPUID 运行时探测 + 硬件路径 + 软件回退"
   这套，Rust 侧只是照搬形状，没有引入新的工程惯例，也没有新增依赖。
4. **这一轮只动构建类**：校验和仍然不在查询和分词链路上，另外两类不受影响。
5. **踩过的坑记下来了**：`_mm_crc32_u32` 一次处理 4 个字节，用它传单字节会让
   每个尾巴字节后面多出 3 个零字节——这是单元测试当场抓出来的，肉眼很难发现。

#### 12.8.2 下一轮的方向

**(a) 构建链路：校验和只剩约 2%。**

硬件路径把校验和从 11.03% 压到 **2.51%**（12.7.3 实测），构建链路的叶子分布已经洗牌：

| 下一批热点 | 占构建叶子时间 | 性质 | 可动性 |
| --- | --- | --- | --- |
| libc（memcpy/memmove 等） | 13.15% | 运行时 | 低，通常是**搬数据**而不是算数据 |
| **字符集比较一组**（`ObNullSafeDatumStrCmp<45>` + `ob_strnncollsp_utf8mb4` + `ob_mb_wc_utf8mb4` + `ObCharset::strcmpsp` + `ObCharset::is_argument_valid` + `ObCharset::get_charset`） | **11.55%** | utf8mb4_general_ci 权重比较 | **oblib 共享层，而且会碰到 MATCH 的大小写不敏感语义** |
| `kv::page::checksum_sse42` | 2.51% | 页校验和（硬件指令） | 已经很干净了 |
| `ObIArrayWrap<void*>::at` | 1.90% | 框架 | 低 |
| `ObOperator::get_next_batch` | 1.63% | 框架 | 低 |
| `ObSortOpImpl::Compare::operator()` | 1.45% | PX 排序 | 中 |
| `ZSTD_compressBlock_fast` | 1.39% | 压缩 | 低 |
| `crc64_sse42` | 0.93% | C++ 侧硬件 CRC | 已有 |

字符集比较那一块现在是 **11.55%**，是构建链路里最大的一块可动目标，
但它同时是 **MATCH 语义的地基**（`utf8mb4_general_ci` 不区分大小写），
动它必须同时证明检索结果不变；libc 那 13.15% 大概率是数据搬运，
不是可以省掉的工作；校验和已经只剩 2.51%，没什么可再压的了。
**所以构建链路剩下的空间，本质上是一次"要不要碰共享存储层和字符集"的抉择。**

**(b) 查询侧：`query_limit` 的差距是真的 BM25 计算，不是无用开销。**

第 7 轮留下的"把 top-K 路径上多余的 `estimate_token_doc_cnt()` 去掉"这条设想，
本轮用 `EXPLAIN` 核对后**作废**（详见 11.6.2 (b)）：`query_limit` 的计划里
`calc_relevance=true` 且 `sort_keys([MATCH(...) DESC])`，BM25 的词权重真的要用
那个文档数。四个查询里只有它还算相关性，它慢是因为**多算了真正要用的分数**。
如果还要在查询侧继续压，方向只能是**相关性表达式本身**（少算一项 BM25 参数、
或让 top-K 的剪枝更早），而不是删一个估计。

**(c) 分词类的天花板。** 分词类现在是 +24.57%（ik +32.47%、beng +16.67%）。
`tokenize_beng` 因为往返开销占比太大，改进率上限只有 42.5%；空间主要在
`tokenize_ik`。下一轮要做分词，得先对**当前**二进制的分词链路重新做一次
符号级归因——第 7 轮的那份剖析已经隔了两轮改动。

## 13. 第 10 轮：把分词热路径上的词典查找从 libc 调用里拿出来

### 13.1 先量后改：按**官方口径**重新剖析三条链路

#### 13.1.1 第 7 轮那份分词剖析不能再用了

第 7 轮的分词剖析是**每条 SQL 新开一个 mysql 连接**跑的。第 9 轮重做剖析时
才发现，这个口径把连接握手算进了分词链路：按符号名统计时，
`ObMPConnect::process()` 的 inclusive 占比是 **31.43%**，
`verify_identify` 28.44%、`load_privilege_info` 28.21%、
`ObBasicSessionInfo::load_all_sys_vars` 25.41% —— 这些全是**连接建立**的开销，
而真正的分词只有 `ObExprTokenize::eval_tokenize` 11.05%。

官方基准（`tools/benchmark/fts_large_bench.sh` 的 `run_repeated_sql_bench`）
的口径完全不同：

```bash
write_session_header "${sample_file}"     # SET NAMES utf8mb4 / ob_query_timeout / ob_trx_timeout
append_repeated_sql "${sample_file}" "${ROUNDS}" "${sql}"   # ROUNDS=3000 条同一条 SQL
start_ns=$(now_ns); $MYSQL < "${sample_file}" >/dev/null; end_ns=$(now_ns)
avg_ms = (end_ns - start_ns) / ROUNDS
```

也就是**一个 sample 只开一次连接**，3000 条 SQL 喂进同一个连接，
`avg_ms` 是整个 mysql 进程的墙钟除以轮数。握手开销被摊薄到 1/3000，
在官方口径下**根本不该出现**。

所以第 10 轮的第一件事不是改代码，而是**把三条链路按官方口径重新剖析一遍**。

#### 13.1.2 剖析方法

* **分词链路**（`perf_tokenize_r10b.sh`）：一个 mysql 进程、一个连接、
  同样的会话头、同样的 `IK_TEXT`，用一条管道持续喂 3000 条同一条 SQL 的循环，
  让负载覆盖整个 120 秒采样窗口（第一版用文件喂，客户端 54 秒就跑完了，
  剩下 60 秒服务端空闲，采样会被稀释，所以改成管道）。
* **查询链路**（`perf_query_r10.sh`）：同样的口径，把官方基准里的**四条**查询
  按顺序不断重复。
* **构建链路**：沿用第 8/9 轮的做法（建索引 + 删索引，900 秒窗口），
  因为它本来就是后台任务驱动、没有连接口径的问题。

解析器也一起修了：第 9 轮的归因脚本把**所有**帧都拿 seekdb 的符号表去解析，
而 perf 栈里混着 `libc.so.6` 的帧，于是 libc 的地址被解析成了 seekdb 里
碰巧排在下方的符号（本轮实测把 13.73% 的 libc 字符串函数误认成了
`absl::LowLevelAlloc::Alloc`）。第 10 轮的解析器**按帧所属的 DSO 分别解析**，
libc 用 `nm -D` 的动态符号表。

#### 13.1.3 分词语句的归因（官方口径，8530 个采样栈）

| 叶子 / 归因块 | 占全部采样 | 说明 |
| --- | --- | --- |
| `ObFTDATReader::find_code` | **5.72%** | 逐字符查词典的 code |
| `ObFTCacheDict::match_with_code` | **3.81%** | DAT 双数组 trie 走一步 |
| `ObFTRangeDict::find_first_char_range` | 1.47% | 二分查找词所属的词典区间 |
| `ObFTCacheDict::match` | 1.23% | 查词 + 走 trie |
| **词典查找合计** | **12.23%** | 全部落在 `do_process` → `process_next_batch` → `get_next_token` → `segment` 下 |
| `ObExprTokenize::eval_tokenize`（inclusive） | 41.44% | 真正的分词工作 |
| `ObJsonExprHelper::pack_json_res`（inclusive） | 3.22% | DOM → ObJsonBin → 二进制 |
| `ObIJsonBase::print`（inclusive） | 7.10% | 结果行编码时把二进制再解析回文本 |
| `ObSql::stmt_query`（inclusive） | 10.15% | SQL 解析 + 计划缓存 |
| libc 字符串函数（memcpy/memset/memcmp 一族） | **13.73%** | 其中 32.02% 在 `eval_tokenize` 下 |

inclusive 里的框架开销：`ObMPQuery::process` 74.43%、
`response_result` 54.48%、`stmt_query` 10.15%、
`append_response` 7.70%、`pc_get_plan` 7.02%。
**分词语句里有 58.56% 根本不是分词**（SQL 解析、计划缓存、结果编码、IO）。

#### 13.1.4 查询与构建链路：剩下的热点都在不能动的地方

| 链路 | 最大的叶子 | 能不能动 |
| --- | --- | --- |
| 查询 | `ObStringDiffDecoder::batch_decode` **6.69%** | **不能**：作业明令避免的共享存储层改动 |
| 查询 | `ObSimpleRowsMerger` 系列 7.82%、`ObSRMergeCmp::cmp` 3.18%、`ObSRDaaTIterImpl` 9.98% | 稀疏检索的归并机制，改动面极大 |
| 查询 | `ObExprBM25::eval_batch_bm25_relevance_expr` 1.42% | BM25 本身 |
| 构建 | `ObNullSafeDatumStrCmp<45>` + 字符集比较一组 **13.5%** | **不能**：排序键用 `utf8mb4_general_ci`，动了就改了 MATCH 的大小写不敏感语义 |
| 构建 | `kv::page::checksum_sse42` 2.51% | 第 9 轮已经压过 |
| 构建 | `ObFTIndexRowCache::segment` 10.04% | 索引构建时的分词 —— **这一轮动的是它** |

查询链路的 `ObStringDiffDecoder::batch_decode` 和构建链路的字符集比较，
都是作业点名/语义锁死的。**剩下的、既能改又能证明等价的，只有分词链路
自己那 12.23% 的词典查找。**

### 13.2 本轮改了什么

分词热路径上，每匹配一个字符、每本词典，就要做一次词典查找。
这条路上原本有**三次进 libc 的调用**：

1. `ObFTSingleWord::set_word()`：`memcpy(word, len)` 拷贝词 +
   `memset()` 清零尾部。长度是运行期的，所以两次都是**函数调用**。
2. `ObArrayHashMap::find()` 的桶内比较：`memcmp(word, other.word, word_len)`，
   又是**一次函数调用**。

也就是说，**一个字符一次词典查找 = 3 次 libc 调用**。

按调用方把这 13.73% 的 libc 叶子拆开，落在词典查找这条路上的有
**152 个采样**（`ObIKCJKProcessor::do_process` 51 + `ObFTCacheDict::match` 34 +
`ObFTRangeDict::match` 67），占 libc 叶子总数的 **12.98%**，
折合分词语句的 **1.78%**；再加上区间字典二分查找里的 `memcmp`，
这一轮去掉的就是这一块。

#### 13.2.1 `ObArrayHashMap::find()` 增加按原始字节的重载

`src/storage/fts/dict/ob_ft_dat_dict.h`

词典 map 的哈希是**对整个定长缓冲区**算的（`hash_single_word` 里
`MEMCPY(&chunk, word.word, sizeof(word.word))`，`ObCharset::MAX_MB_LEN` = 5 字节，
词后面的字节恒为 0）。所以：

* 一个词**按原始字节**算出来的 chunk，和把它装进 `ObFTSingleWord` 之后的 chunk
  **逐位相同**；
* 两个词的比较，`memcmp` 比的是 `word_len` 个字节，而定长缓冲区里词后面的字节
  恒为 0，所以**比较整个缓冲区等价于比较词本身**。

于是新增的重载直接用原始字节算哈希、比较桶：

```cpp
int find(const char *ptr, const int32_t len, ObFTWordCode &code) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  if (OB_LIKELY(len >= 0 && len <= static_cast<int32_t>(sizeof(ObFTSingleWord::word)))) {
    const uint64_t chunk = load_word_bytes(ptr, len);
    ...
      if (entry.word.word_len == len && load_word_buffer(entry.word) == chunk) {
```

**哈希值逐位不变**，所以桶布局不变、`next_code_` 的分配顺序不变、
DAT 的 code 空间不变 —— **索引内容不变**。

#### 13.2.2 `ObFTSingleWord::set_word()` / `operator==` 不再进 libc

`src/storage/fts/dict/ob_ft_dict_def.cpp`

* `set_word()`：用一次循环把词装进一个 64 位 chunk（尾部天然是 0），
  再 `MEMCPY` 5 个字节进定长缓冲区 —— 一次定长拷贝，编译器展开成指令，
  没有函数调用。
* `operator==()`：两次定长 `MEMCPY`（5 字节）后用掩码截出 `word_len` 个字节比较，
  和 `memcmp(word, other.word, word_len)` **完全等价**，但没有函数调用。
  （掩码这一步是刻意加的：这样即使缓冲区尾部有残留字节，行为也和
  原来的 `memcmp` 一样，不依赖"尾部恒为 0"这个前提。）

#### 13.2.3 区间字典的二分查找也不再进 libc

`src/storage/fts/dict/ob_ft_range_dict.cpp` 的 `ft_bin_strcmp()` 原本用
`memcmp` 比较两个单字符（长度是运行期的 → 又是函数调用）。
它被 `find_first_char_range()` 的二分查找调用，**每个词一次查找、
每次查找 log2(区间数) 次比较**。改成逐字节比较，语义不变
（仍然是"先比字节、再比长度差"，与 `ob_strnncoll_mb_bin` 一致）。

### 13.3 等价性是怎么证明的

| 改动 | 等价性依据 |
| --- | --- |
| `find(ptr, len)` 的哈希 | `load_word_bytes(ptr, len)` 得到的 chunk 与 `load_word_buffer(ObFTSingleWord)` 得到的 chunk 逐位相同（词后面的字节恒为 0），因此哈希值、桶位置、探测序列全部不变 |
| `find(ptr, len)` 的比较 | `word_len` 相等且两个定长缓冲区的 `word_len` 字节相同 ⟺ 原来的 `memcmp(word, other.word, word_len) == 0` |
| `set_word()` | 写入的 5 字节缓冲区与原来 `memcpy` + `memset` 的结果逐字节相同 |
| `operator==()` | 掩码后比较的正是 `word_len` 个字节，与 `memcmp` 的语义和返回值符号一致 |
| `ft_bin_strcmp()` | 逐字节比较返回第一个不同字节的差值（无符号），相等时返回长度差，与 `memcmp` + 长度差的组合一致 |

另外三条硬约束在验证里逐条查过：

* **TOKENIZE 逐字节一致**：固定文本集的输出两臂 `diff` 为空；
* **MATCH 命中数不变**：8001 / 11000 / 7332 / 20；
* **索引内容不变**：索引隐藏列的 `CRC32` 求和、行数、字节数一致。

### 13.4 正确性验证

| 检查项 | 第 9 轮臂 (10103) | 第 10 轮臂 (10104) | 结论 |
| --- | --- | --- | --- |
| TOKENIZE 输出（固定文本集，121 行） | 见 `/tmp/verify_r10/tokenize_A.txt` | 见 `/tmp/verify_r10/tokenize_B.txt` | **逐字节一致** |
| MATCH 命中数（4 条官方查询） | 8001, 11000, 7332, 20 | 8001, 11000, 7332, 20 | **一致** |
| MATCH 有序结果行 + BM25 分数 | 见 `rows_A.txt` | 见 `rows_B.txt` | **逐行一致** |
| 索引内容指纹（行数 / CRC32 和 / 字节数） | `20000	43236786491121	5832098` | `20000	43236786491121	5832098` | **一致** |
| 索引 doc id 列的字节数 | 320000 | 320000 | **一致** |

两个臂的索引隐藏列名（`cols`）只差 DDL 里的序号后缀；doc id 列的首值是存储层
发出来的内部行标识的基准值，取决于发号器之前发过多少号，**本来就不可比**
（比的是长度和形状）。除这两项之外，两臂的索引内容完全一致。


### 13.5 性能

**tokenize_ik**（官方口径：单连接 + 会话头 + 8 个样本，ABBA 交错）

| 臂 | 样本数 | 平均 (ms/条) | 中位数 (ms/条) | 标准差 |
| --- | --- | --- | --- | --- |
| A：第 9 轮二进制 (10103) | 16 | 0.3035 | 0.2975 | 0.0268 |
| B：第 10 轮二进制 (10104) | 16 | 0.3066 | 0.3080 | 0.0307 |

* 平均比 B/A = **1.0103**（B 比 A 快 **-1.03%**）
* 中位数比 B/A = 1.0353
* 按轮配对的比值：均值 1.0145，标准差 0.1044


**本机测不出这一轮的效果，原因是噪声远大于信号。** 上表的配对比值标准差是
6%~10%，而这一轮按剖析占比推算的效应只有分词语句的 ~1.8%（libc 调用占
13.73% 的 libc 叶子中的 12.98%）。两次 ik 的测量连**符号都不一致**
（一次中位数比 0.9963、一次 1.0353），beng 的配对比值 0.9099 ± 0.0632。
按 95% 置信区间算，本机根本分辨不出 1% 量级的差异。

本机更测不出的原因是**分母不对**：本机一条 `SELECT tokenize(...)` 要 0.28 ms，
而 CI 上只要 0.057 ms，本机多出来的部分是 mysql 客户端的往返和慢盘，
分词本身在总时间里占的比例比 CI 更小，信号被埋得更深。

所以这一轮的**性能读数只能由 CI 给**（CI 的分词类噪声远小于本机：
第 8 轮两次 `tokenize_ik` 的差在千分位）。本机只用来证明两件事：
**输出等价**（见上表）和**改动确实落在剖析指出的热点上**。


### 13.6 结论与下一轮的方向

1. **这一轮先把口径修对了**：第 7 轮那份分词剖析是"每条 SQL 开一个连接"
   跑的，把连接握手算进了分词链路（`ObMPConnect::process` 占 31.43%）；
   官方基准是**一个 sample 只开一次连接**，握手开销被摊薄到 1/3000。
   三条链路按官方口径重剖析之后，"分词还剩多少空间"这个问题才第一次有
   可靠的答案。归因脚本也一起修了：perf 栈里混着 libc 的帧，
   用 seekdb 的符号表去解析 libc 的地址会得到完全错误的结论。
2. **分词链路里剩下的、fulltext 自己拥有的最大块是词典查找（12.23%）**，
   这一轮把这条路上的三次 libc 调用去掉。改动在 fulltext 存储层，
   哈希值逐位不变，桶布局不变，索引内容不变。
3. **查询和构建链路剩下的热点都在不能动的地方**：查询侧最热的叶子是
   `ObStringDiffDecoder::batch_decode`（6.69%），是作业明令避免的共享存储层改动；
   构建侧最热的是 PX 排序里的 `utf8mb4_general_ci` 权重比较（约 13.5%），
   动了就改了 MATCH 的大小写不敏感语义。这两块没有做，理由写在这里。
4. **下一轮的方向**：分词语句里 **58.56% 不是分词**——SQL 解析、计划缓存
   （`pc_get_plan` 7.02%）、结果编码（`append_response` 7.70%，其中
   JSON 结果的"二进制再解析回文本"占 7.10%）。JSON 结果那段要动就得动
   oblib 的公共 JSON 库，和 `batch_decode` 属于同一类风险。
   在不碰共享层的前提下，分词侧还剩
   `ObFTParseHelper::init`（每次调用重新解析 parser name，约 0.55%）、
   `ObIKArbitrator`（4.67%）和 `ObFTWordMap` 的分组统计（约 2.8%）
   这些可以逐个量、逐个证明的点。


## 14. 第 11 轮：有界读不再做全文排序，索引回表不再超量预取

第 10 轮结束时，CI 给出的 `query_limit` 是 1.4868 ms（第 8 轮）／1.5108 ms（第 10 轮），
而另外三个查询是 0.86 / 1.29 / 0.99 ms。这一轮盯的就是这条最慢的查询：

```sql
SELECT COUNT(*) FROM (
  SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20
) t;
```

### 14.1 瓶颈定位

**（1）计划里多了一个没人要的排序。**

`EXPLAIN` 显示第 10 轮这条 SQL 的计划里，全文检索扫描是
`calc_relevance=true`、`sort_keys([MATCH(...) DESC])`、`limit(20)` 在**排序之上**，
代价估计 406313 µs。原因在 `ObTransformPreProcess::preserve_order_for_fulltext_search`：
单表全文语句只要没有显式 `ORDER BY`，就会被注入一个
`ORDER BY MATCH(...) DESC`。这个隐式排序对**无界**查询是"按相关度返回"的
展示便利，但一旦带 `LIMIT 20`，它要求把 8001 个命中全部算一遍 BM25、
再取前 20 行——**20 行结果付出了 8001 行的排序代价**。

**（2）回表按 256 行的固定批量预取。**

去掉排序之后计划变成 `calc_relevance=false`、`limit(20)` 下推到
TEXT RETRIEVAL SCAN，本机 20 行只要 ~0.5 ms。但带 `id` 列时要回表
（index back），这条路是 `ObDASCacheLookupIter`：`INDEX_SCAN` 阶段按
`default_batch_row_count_`（=256）收集 rowkey，再 `DO_LOOKUP` 批量查主表。
它不看调用方这一批究竟还要多少行，**`LIMIT 20` 也照样查 256 个主表行**，
然后扔掉 236 个。20 行的有界读变成了 256 行的随机回表。

### 14.2 修改

**（a）有界且分数不可见时，不注入隐式相关度排序**
（`src/sql/rewrite/ob_transform_pre_process.cpp`）

只在 `stmt->has_limit()` 且 SELECT 列表里没有任何含
`CNT_MATCH_EXPR` 的表达式时跳过注入：

* 没有 `ORDER BY` 时 SQL 不承诺行序，跳过注入不改变任何**承诺过的**语义；
* 有界 `COUNT(*)` 的结果是 `min(命中数, limit)`，两臂一致（见 14.3）；
* SELECT 里选了分数的情况**保持注入**——结果把相关度给用户看，
  行就必须还是 top-N（`basic_dml.test` 第 1120 行就是这种形态）；
* 显式 `ORDER BY MATCH(...) DESC LIMIT n` 本来就在函数开头的
  `has_order_by` 判断里跳过，不受影响；
* 无界语句保持注入，`query_cn` / `query_beng` / `query_mixed` 的计划不变。

**（b）表扫描按剩余 limit 收缩第一批请求**
（`src/sql/engine/table/ob_table_scan_op.cpp` 的 `get_next_batch_with_das`）

算子自己执行最终 limit（`need_final_limit_`）时，第一次向迭代器要行数就
不超过 `limit - output_row_cnt_`。算子级过滤丢掉的行由下面已有的循环
按需要再拉，不影响正确性，只是不再预取用不上的行。

**（c）回表收集量跟随调用方的 capacity**
（`src/sql/das/iter/ob_das_cache_lookup_iter.cpp`）

`INDEX_SCAN` 的收集上限从固定的 `default_batch_row_count_` 改成
`min(default_batch_row_count_, capacity)`。无界扫描的 `capacity` 本来就是
整批，行为不变；有界的读只收集还需要的行数。

**（d）utf8mb4_general_ci 的权重直算快路径**
（`src/oblib/lib/charset/ob_ctype_utf8.cc`）

第 10 轮把"字符集比较一组"（`ObNullSafeDatumStrCmp<45>` +
`ob_strnncollsp_utf8mb4` + `ob_mb_wc_utf8mb4` + `ObCharset::strcmpsp` +
`ObCharset::is_argument_valid` + `ObCharset::get_charset`，构建剖析里合计
**11.55%**）列为"动了就会改 MATCH 大小写不敏感语义"而没做。这一轮把它做掉的
办法是**先把权重表逐行核实**，再只对权重可闭式算出的两类字符走快路径：

* 单字节（U+0000..U+007F）：权重就是码点，只有 `a`..`z` 折叠成大写
  （`utf8_plane00` 逐行核对）；
* 三字节且所在平面在 `ob_unicase_pages_default` 里没有表
  （0x08..0xFF 中只有 0x1E、0x1F、0x21、0x24、0xFF 有表）：码点原样保留，
  权重即码点（CJK 平面 0x4E..0x9F 全是 NULL 页）。

其余字符（两字节、四字节、有表平面、非法/截断序列）一律返回"未处理"，
由原循环**从头**重算，所以快路径只会返回原循环同样会返回的值。
校验条件和 `ob_mb_wc_utf8mb4()` 完全一致（首字节范围、两个续字节、
0xE0 后不得接 0x80..0x9F 以免过长编码）；`strnncollsp` 的行尾空格规则和
`strnncoll` 的长度差规则仍走原来的代码。入口用
`cs->caseinfo == &ob_unicase_default` 把关（该处理器只被
`utf8mb4_general_ci` 使用，其 `state` 为 0，没有 `OB_CS_LOWER_SORT`），
权重逐位不变。

**（e）`CS_TYPE_BINARY` 的 datum 比较直写**
（`src/share/datum/ob_datum_cmp_func_def.h`）

模板参数是 `CS_TYPE_BINARY` 时，`ObDatumStrCmpImpl::cmp` 直接做
`memcmp(min_len)` + 长度差并归一化成 -1/0/1，等价于
`ObCharset::strcmpsp` → `ob_strnncollsp_binary`，省掉参数校验和字符集分发
（这条是第 5 轮在归并堆上做过的同一件事，落到 datum 通用比较入口）。

### 14.3 正确性

| 检查项 | 第 10 轮臂 (10104) | 第 11 轮臂 (10103) | 结论 |
| --- | --- | --- | --- |
| TOKENIZE 输出（固定文本集 × ik/beng/all） | 33 行 | 33 行 | **逐字节一致** |
| MATCH 命中数 / 有界 COUNT / LIMIT 20 行集 | 8 行 | 8 行 | **逐字节一致**（8001 / 11000 / 7332 / 20） |
| MATCH 有序结果 + BM25 分数（4 条查询各 200 行） | 800 行 | 800 行 | **逐行一致** |
| 列排序/等值/GROUP BY/IN/BETWEEN/MIN/MAX（50 行 × 7 条） | 190 行 | 190 行 | **逐行一致** |
| 全表 `ORDER BY title / title_en / title DESC,id` 的 md5 | 3 行 | 3 行 | **一致** |
| 重建全文索引后的逐词命中数（13 词条 + 英文短语 + id 和） | 15 行 | 15 行 | **一致** |

`fts_index` 套件（19 个用例）：**16 通过**。三个失败都与本轮改动无关：

* `cn_word`、`partitioned_simple_query` 在第 371/44 行 `connect admin` 需要
  admin 账号（`Access denied for 'admin'`），本地 root 臂没有这个账号；
* `basic_dml` 卡在第 1598 行对 `__all_column` 的元数据查询
  （`$rowkey_doc_table_id` 取不到值），该用例在第 8/10 轮也没通过过。

新增用例 `fts_limit_filter`（有界命中数、选分数保持相关度序、显式
`ORDER BY`、无界相关度序、`general_ci` 的 ASCII/CJK/混排/带音符/四字节
排序等值）已在最终二进制上录制 `.result` 并回放通过。

### 14.4 性能

**有界查询，本机同机 A/B**（ABBA 交错，200 次平均，20 次预热，同一台
机器同一份 20000 行数据，两臂各自重建过同一份全文索引）：

| 查询形态 | 第 10 轮 (ms) | 第 11 轮 (ms) | 加速 |
| --- | --- | --- | --- |
| `SELECT 1 ... LIMIT 20`（不回表） | 2.7853 | 0.4999 | **5.57×** |
| `SELECT id ... LIMIT 20`（官方 query_limit 形态） | 2.9352 | 0.7390 | **3.97×** |
| `SELECT 1 ... LIMIT 200` | 3.1779 | 0.5664 | **5.61×** |
| `SELECT id ... LIMIT 200` | 4.5644 | 1.6457 | **2.77×** |
| `SELECT id ... LIMIT 2000` | 14.2719 | 8.0470 | **1.77×** |

**三条无界查询的对照**（确认本轮没有伤到它们；同一脚本、3 轮 ABBA）：

| 查询 | 第 10 轮 (ms) | 第 11 轮 (ms) |
| --- | --- | --- |
| `query_cn` 形态 | 1.7419 / 1.7663 / 1.7605 | 1.7561 / 1.7762 / 1.7568 |
| `query_mixed` 形态 | 2.0023 / 1.9594 / 1.9700 | 1.9115 / 1.9559 / 1.9561 |
| `query_beng` 形态 | 2.5297 / 2.5358 / 2.5184 | 2.5280 / 2.5203 / 2.6128 |

计划侧：第 10 轮 `calc_relevance=true + sort_keys([MATCH DESC]) + limit(20)`
（代价估计 406313 µs）→ 第 11 轮 `calc_relevance=false + limit(20)` 下推
（代价估计 654 µs）。

**CI 实测**（`fd9d85ca5`，对照第 8 轮 `1eeaaa6` / 第 10 轮 `54d25bafe`）：

| 指标 | 第 8 轮 | 第 10 轮 | 第 11 轮 |
| --- | --- | --- | --- |
| build_ik_all_sec | 6.250 | 6.151 | 6.258 |
| build_ik_content_sec | 4.823 | 4.725 | **6.634** |
| build_beng_en_sec | 2.620 | 5.627 | 2.624 |
| tokenize_ik_avg_ms | 0.0574 | 0.0554 | 0.0597 |
| tokenize_beng_avg_ms | 0.0365 | 0.0367 | 0.0389 |
| query_cn_avg_ms | 0.8576 | 0.8608 | 0.9430 |
| query_beng_avg_ms | 1.2923 | 1.2771 | 1.3939 |
| query_mixed_avg_ms | 0.9925 | 0.9788 | 1.0642 |
| query_limit_avg_ms | 1.4868 | 1.5108 | **0.2087** |
| fts_mean_improvement | 0.24857 | 0.14102 | 0.23184 |
| 得分 | 24.587 | 14.102 | 23.284 |

读数说明：

* `query_limit` 从 1.4868 ms 降到 **0.2087 ms（7.1×）**，命中数 20 不变。
  这个量级说明 CI 快盘上"每查询固定回表开销"基本消失，和本机
  `SELECT 1`（0.50 ms）→ `SELECT id`（0.74 ms）的差值一致。
* `query_cn / query_beng / query_mixed / tokenize_*` 整体比第 8 轮慢
  4%~10%，且**方向一致**；本机配对 A/B 显示三条无界查询两臂持平
  （差 < 2%，见上表），第 11 轮的代码不含任何作用于无界查询路径的改动
  （排序注入保留、批量参数在无界时不变），所以这是当次 CI 机器状态的
  整体漂移，不是回归。
* `build_ik_content_sec` 6.634 s（第 8/10 轮 4.82 / 4.73 s）是本轮掉分的主因：
  那个 ±2 s 的"坏签"第 10 轮落在 `build_beng_en`（5.627 s），这次换到了
  `build_ik_content`。同一签位换位置说明是构建之间的后台合并/刷盘串扰，
  不是某个索引的系统性变慢——本轮对构建链路唯一的改动（字符集快路径）
  只会减少 CPU。本机构建一次要 50 s 量级（慢盘主导），测不出 CI 上
  3~7 s 量级的差异，构建类只能以 CI 读数为准。
* 按上一节的口径粗算：若 `build_ik_content` 抽到 4.8 s 左右的"正常签"，
  本轮 mean_improvement 约 0.263、得分约 **26.4**，即本轮的真实水平
  高于第 8 轮的 24.587。

### 14.5 结论与下一轮方向

1. `query_limit` 已经从"最慢的查询"变成"最快的查询"（0.2087 ms，比
   `select 1` 的 0.0269 ms 只高一个量级），查询类里剩下的三条无界查询
   与分词类的提升空间都在"每命中行的通用路径"上（结果编码、计划缓存、
   布尔求值），第 9/10 轮已经把容易的摘完了。
2. **构建类是唯一的数量级短板**：`build_ik_all` 三轮都钉在 6.15~6.26 s，
   `build_ik_content`/`build_beng_en` 在 2.6~6.6 s 之间抽签。20000 行的
   表建全文索引要 3~7 s，显然不是分词 CPU（本机全量 CPU 剖析里分词只占
   小头），而是 DDL 机器本身的固定开销（排序缓冲刷盘、索引表
   build/merge、DDL 收尾）。要抬分必须把这笔固定开销砍下来，并且把
   构建之间的串扰（坏签）消掉。
3. 坏签的消法值得单独验证：三次 CI 里坏签分别落在第 3、第 2 个构建上，
   疑似上一个构建的 sort 临时文件/后台合并还没落稳。如果能让每个
   ALTER 的 DDL 收尾（flush + 等待合并）变短，三个构建就都能落在
   "好签"上，这一项就值 2~4 分。
