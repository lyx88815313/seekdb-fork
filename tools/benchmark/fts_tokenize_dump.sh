#!/bin/bash
# Dump TOKENIZE() output for a set of texts so the baseline and the candidate
# can be compared byte for byte.
# usage: fts_tokenize_dump.sh <port> <outfile>
set -uo pipefail
PORT="${1:-10100}"
OUT="${2:-/tmp/tokenize_dump.txt}"
MYSQL="mysql -h127.0.0.1 -P${PORT} -uroot -N -s --default-character-set=utf8mb4"

TEXTS=(
  "OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术。"
  "The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path."
  "全文索引 分词器"
  "OceanBase 稳定 database"
  "倒排索引 tokenizer"
  "数据库"
  "a"
  "A"
  "ABC"
  "hello world"
  "hello, world! 123 456"
  "2024年1月1日，第3季度营收增长10.5%"
  "混合mixed文本text混合"
  "①②③ ⑴ ⅰ"
  "😀😃 emoji 测试 🚀"
  "   "
  "a b c d e f g h i j k l m n o p q r s t u v w x y z"
  "中华人民共和国中央人民政府今天成立了"
  "IKEA的创始人英格瓦·坎普拉德"
  "strlen与strncpy的区别"
  "C++语言与C#语言"
  "XML解析与JSON解析"
  "IPv4和IPv6协议"
  "2020-01-01T00:00:00Z"
  "temperature is 36.6°C"
  "don't can't won't"
  "AT&T IBM Microsoft"
  "foo_bar baz-qux quux"
  "测试测试测试测试测试测试测试测试测试测试"
)

: > "$OUT"
for t in "${TEXTS[@]}"; do
  {
    echo "### TEXT: ${t}"
    $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('${t}', 'ik');" 2>&1
    echo "### TEXT: ${t} (beng)"
    $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('${t}', 'beng');" 2>&1
  } >> "$OUT"
done
# also exercise the parser property parsing path
{
  echo "### WITH PARSER PROPERTY"
  $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('全文索引的分词器测试', 'ik', '{\"min_token_size\":2}');" 2>&1
  $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('Full text indexing test', 'beng', '{\"min_token_size\":2}');" 2>&1
  $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('全文索引的分词器测试', 'ngram', '{\"ngram_token_size\":2}');" 2>&1
  $MYSQL -e "SET NAMES utf8mb4; SELECT tokenize('Full text indexing test', 'space');" 2>&1
} >> "$OUT"
echo "wrote $OUT"
