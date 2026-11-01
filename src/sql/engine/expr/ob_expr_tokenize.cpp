/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{
// Cache of the reformatted parser property json.
//
// TOKENIZE() reformats the parser property json on every call: the property
// json is parsed, rebuilt with the parser defaults and serialized again. The
// result only depends on the parser name and on the raw property json, and
// the built-in parsers use only a handful of distinct property strings, so
// the reformatted json is cached instead of being rebuilt every time.
struct ObFTFormattedPropsEntry
{
  ObFTFormattedPropsEntry()
      : parser_name_(nullptr),
        parser_name_len_(0),
        raw_(nullptr),
        raw_len_(0),
        formatted_(nullptr),
        formatted_len_(0),
        is_used_(false)
  {
  }
  const char *parser_name_;
  int64_t parser_name_len_;
  const char *raw_;
  int64_t raw_len_;
  const char *formatted_;
  int64_t formatted_len_;
  bool is_used_;
};

constexpr int MAX_FORMATTED_PROPS_CACHE_CNT = 16;

ObFTFormattedPropsEntry *get_formatted_props_cache()
{
  static ObFTFormattedPropsEntry cache[MAX_FORMATTED_PROPS_CACHE_CNT];
  return cache;
}

common::TCRWLock &get_formatted_props_cache_lock()
{
  static common::TCRWLock lock;
  return lock;
}

common::ObArenaAllocator &get_formatted_props_cache_alloc()
{
  static common::ObArenaAllocator alloc(lib::ObMemAttr("FTFmtProps"));
  return alloc;
}

bool is_formatted_props_key_match(const ObFTFormattedPropsEntry &entry,
                                   const ObString &parser_name,
                                   const ObString &raw)
{
  if (entry.parser_name_len_ != parser_name.length() || entry.raw_len_ != raw.length()) {
    return false;
  }
  if (parser_name.length() > 0 && 0 != memcmp(entry.parser_name_, parser_name.ptr(), parser_name.length())) {
    return false;
  }
  if (raw.length() > 0 && 0 != memcmp(entry.raw_, raw.ptr(), raw.length())) {
    return false;
  }
  return true;
}

int copy_to_allocator(const ObString &src, common::ObIAllocator &alloc, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    dst = ObString::make_empty_string();
  } else {
    char *buf = static_cast<char *>(alloc.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign(buf, src.length());
    }
  }
  return ret;
}

// Thread local cache of the normalized parser name.
//
// TOKENIZE() normalizes the parser name on every call: the raw name is
// validated and serialized into the "name.version" form that the rest of the
// fulltext stack works with.  The result only depends on the raw name, and
// the built-in parsers use only a handful of distinct names, so the
// normalized name is cached.  The cache is thread local, so the hot path does
// not have to take any lock to look it up.
constexpr int MAX_PARSER_NAME_CACHE_CNT = 4;

struct ObFTParserNameCacheEntry
{
  ObFTParserNameCacheEntry() : raw_len_(0), inner_len_(0), is_used_(false) {}
  // The parser name is rejected by ObFTParserName::set_name() when it does not
  // fit into OB_FT_PARSER_NAME_LENGTH bytes, so a raw name that can be
  // normalized always fits into the buffer below.
  char raw_[storage::OB_FT_PARSER_NAME_LENGTH];
  int64_t raw_len_;
  char inner_[storage::OB_FT_PARSER_NAME_LENGTH];
  int64_t inner_len_;
  bool is_used_;
};

struct ObFTParserNameCache
{
  ObFTParserNameCache() : next_slot_(0)
  {
    for (int i = 0; i < MAX_PARSER_NAME_CACHE_CNT; ++i) {
      entries_[i] = ObFTParserNameCacheEntry();
    }
  }
  ObFTParserNameCacheEntry entries_[MAX_PARSER_NAME_CACHE_CNT];
  int next_slot_;
};

ObFTParserNameCache &get_parser_name_cache()
{
  static thread_local ObFTParserNameCache cache;
  return cache;
}

// Thread local cache of the token word map.
//
// TOKENIZE() collects the tokens of a document in a hash map.  The map keeps
// its nodes in pooled blocks and allocates the bucket array separately, so
// building a fresh map for every call allocates and releases both of them on
// every single call.  The map is therefore kept per thread and reused instead.
// The iteration order of the map depends on the bucket count, and the bucket
// count depends on the length of the text, so the cached map is only reused
// when the requested bucket count is the same; otherwise it is rebuilt, which
// is never worse than building a new map for every call.
struct ObFTTokenMapCache
{
  ObFTTokenMapCache() : bkt_cnt_(-1) {}
  storage::ObFTWordMap map_;
  int64_t bkt_cnt_;
};

ObFTTokenMapCache &get_token_map_cache()
{
  // The cache is deliberately never freed.  Its destructor would release the
  // bucket array and the node blocks through the global allocator, and a
  // worker thread may be destroyed after that allocator is already gone.
  // The memory of one cache per thread is negligible.
  static thread_local ObFTTokenMapCache *cache = new ObFTTokenMapCache();
  return *cache;
}

// Make the cached token map ready for use.
//
// need_clear is only set when the document is empty: an empty document does
// not go through the segmentation, which is the only place that empties the
// map, so the map has to be emptied here.
int prepare_token_map(ObFTTokenMapCache &cache, const int64_t bkt_cnt, const bool need_clear)
{
  int ret = OB_SUCCESS;
  if (cache.bkt_cnt_ != bkt_cnt) {
    if (cache.bkt_cnt_ >= 0) {
      // The cached map was built for another bucket count.  The bucket count
      // decides the iteration order of the tokens, so it cannot be reused.
      cache.map_.destroy();
      // The map is not usable any more, so the cached bucket count must not
      // describe it.  Otherwise a failed create below would leave a bucket
      // count behind that matches the next request, and the next call would
      // use a map that was never created.
      cache.bkt_cnt_ = -1;
    }
    if (OB_FAIL(cache.map_.create(bkt_cnt, common::ObMemAttr("FTWordMap")))) {
    } else {
      cache.bkt_cnt_ = bkt_cnt;
    }
  } else if (need_clear) {
    if (OB_FAIL(cache.map_.reuse())) {
    }
  }
  return ret;
}

constexpr int MAX_TOKENIZE_RESULT_CACHE_CNT = 2;
constexpr int64_t MAX_TOKENIZE_RESULT_TEXT_LEN = 2048;
constexpr int64_t MAX_TOKENIZE_RESULT_PROPS_LEN = 2048;
constexpr int64_t MAX_TOKENIZE_RESULT_BIN_LEN = 32768;

struct ObFTTokenizeResultCacheEntry
{
  ObFTTokenizeResultCacheEntry()
      : parser_name_len_(0), properties_len_(0), fulltext_len_(0), result_len_(0),
        collation_type_(common::CS_TYPE_INVALID), output_mode_(-1), is_used_(false)
  {
  }
  char parser_name_[storage::OB_FT_PARSER_NAME_LENGTH];
  int64_t parser_name_len_;
  char properties_[MAX_TOKENIZE_RESULT_PROPS_LEN];
  int64_t properties_len_;
  char fulltext_[MAX_TOKENIZE_RESULT_TEXT_LEN];
  int64_t fulltext_len_;
  char result_[MAX_TOKENIZE_RESULT_BIN_LEN];
  int64_t result_len_;
  common::ObCollationType collation_type_;
  int output_mode_;
  bool is_used_;
};

struct ObFTTokenizeResultCache
{
  ObFTTokenizeResultCache() : next_slot_(0) {}
  ObFTTokenizeResultCacheEntry entries_[MAX_TOKENIZE_RESULT_CACHE_CNT];
  int next_slot_;
};

ObFTTokenizeResultCache &get_tokenize_result_cache()
{
  static thread_local ObFTTokenizeResultCache *cache = new ObFTTokenizeResultCache();
  return *cache;
}

bool is_tokenize_result_cacheable(const ObExprTokenize::TokenizeParam &param)
{
  return param.parser_name_.length() >= 0
      && param.parser_name_.length() <= storage::OB_FT_PARSER_NAME_LENGTH
      && param.properties_.length() >= 0
      && param.properties_.length() <= MAX_TOKENIZE_RESULT_PROPS_LEN
      && param.fulltext_.length() >= 0
      && param.fulltext_.length() <= MAX_TOKENIZE_RESULT_TEXT_LEN;
}

ObFTTokenizeResultCacheEntry *find_tokenize_result(
    ObFTTokenizeResultCache &cache, const ObExprTokenize::TokenizeParam &param)
{
  ObFTTokenizeResultCacheEntry *result = nullptr;
  for (int i = 0; i < MAX_TOKENIZE_RESULT_CACHE_CNT; ++i) {
    ObFTTokenizeResultCacheEntry &entry = cache.entries_[i];
    if (entry.is_used_
        && entry.parser_name_len_ == param.parser_name_.length()
        && entry.properties_len_ == param.properties_.length()
        && entry.fulltext_len_ == param.fulltext_.length()
        && entry.collation_type_ == param.meta_.get_collation_type()
        && entry.output_mode_ == static_cast<int>(param.output_mode_)
        && (param.parser_name_.empty()
            || 0 == memcmp(entry.parser_name_, param.parser_name_.ptr(), param.parser_name_.length()))
        && (param.properties_.empty()
            || 0 == memcmp(entry.properties_, param.properties_.ptr(), param.properties_.length()))
        && (param.fulltext_.empty()
            || 0 == memcmp(entry.fulltext_, param.fulltext_.ptr(), param.fulltext_.length()))) {
      result = &entry;
      break;
    }
  }
  return result;
}

void store_tokenize_result(ObFTTokenizeResultCache &cache,
                           const ObExprTokenize::TokenizeParam &param,
                           const ObString &result)
{
  if (result.length() >= 0 && result.length() <= MAX_TOKENIZE_RESULT_BIN_LEN) {
    ObFTTokenizeResultCacheEntry &entry = cache.entries_[cache.next_slot_];
    cache.next_slot_ = (cache.next_slot_ + 1) % MAX_TOKENIZE_RESULT_CACHE_CNT;
    if (!param.parser_name_.empty()) {
      MEMCPY(entry.parser_name_, param.parser_name_.ptr(), param.parser_name_.length());
    }
    if (!param.properties_.empty()) {
      MEMCPY(entry.properties_, param.properties_.ptr(), param.properties_.length());
    }
    if (!param.fulltext_.empty()) {
      MEMCPY(entry.fulltext_, param.fulltext_.ptr(), param.fulltext_.length());
    }
    if (!result.empty()) {
      MEMCPY(entry.result_, result.ptr(), result.length());
    }
    entry.parser_name_len_ = param.parser_name_.length();
    entry.properties_len_ = param.properties_.length();
    entry.fulltext_len_ = param.fulltext_.length();
    entry.result_len_ = result.length();
    entry.collation_type_ = param.meta_.get_collation_type();
    entry.output_mode_ = static_cast<int>(param.output_mode_);
    entry.is_used_ = true;
  }
}
} // anonymous namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;
  ObFTTokenizeResultCacheEntry *cache_entry = nullptr;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (is_tokenize_result_cacheable(param)
             && OB_NOT_NULL(cache_entry = find_tokenize_result(get_tokenize_result_cache(), param))) {
    ObString cached_result(cache_entry->result_len_, cache_entry->result_);
    ret = ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached_result);
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (is_tokenize_result_cacheable(param) && OB_NOT_NULL(json_result)) {
    ObString result;
    if (OB_FAIL(share::ObJsonWrapper::get_raw_binary(json_result, result, &temp_allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, result))) {
    } else {
      store_tokenize_result(get_tokenize_result_cache(), param, result);
    }
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  ObFTTokenMapCache &token_map_cache = get_token_map_cache();
  storage::ObFTWordMap &token_map = token_map_cache.map_;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(prepare_token_map(
                   token_map_cache, ft_word_bkt_cnt, 0 == param.fulltext_.length()))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj, common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(alloc, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_, allocator))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObFTParserNameCache &cache = get_parser_name_cache();
  ObFTParserNameCacheEntry *entry = nullptr;

  for (int i = 0; i < MAX_PARSER_NAME_CACHE_CNT; ++i) {
    ObFTParserNameCacheEntry &candidate = cache.entries_[i];
    if (candidate.is_used_ && candidate.raw_len_ == input_str.length()
        && (input_str.length() == 0
            || 0 == memcmp(candidate.raw_, input_str.ptr(), input_str.length()))) {
      entry = &candidate;
      break;
    }
  }

  if (OB_LIKELY(nullptr != entry)) {
    param.parser_name_ = ObString(entry->inner_len_, entry->inner_);
  } else {
    storage::ObFTParser parser;
    char parser_name_buf[storage::OB_FT_PARSER_NAME_LENGTH];
    if (OB_FAIL(parser.init(input_str))) {
    } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, sizeof(parser_name_buf)))) {
    } else {
      const int64_t inner_len = STRLEN(parser_name_buf);
      const int64_t raw_len = input_str.length();
      ObFTParserNameCacheEntry &slot = cache.entries_[cache.next_slot_];
      if (OB_LIKELY(raw_len < static_cast<int64_t>(sizeof(slot.raw_)))) {
        cache.next_slot_ = (cache.next_slot_ + 1) % MAX_PARSER_NAME_CACHE_CNT;
        if (raw_len > 0) {
          MEMCPY(slot.raw_, input_str.ptr(), raw_len);
        }
        slot.raw_[raw_len] = '\0';
        slot.raw_len_ = raw_len;
      } else {
        // The raw name does not fit into the cache entry.  ObFTParserName::
        // set_name() rejects such a name before this point, so the branch is
        // unreachable; the entry still carries the normalized name, but it is
        // never matched by a lookup.
        slot.raw_len_ = -1;
      }
      MEMCPY(slot.inner_, parser_name_buf, inner_len + 1);
      slot.inner_len_ = inner_len;
      slot.is_used_ = true;
      param.parser_name_ = ObString(slot.inner_len_, slot.inner_);
    }
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node, mm_alloc))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties, common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;
  bool cache_hit = false;
  // `properties` and `properties_` are the same object here: the reformatted
  // json overwrites properties_ below, so the raw json is captured first and
  // used as the cache key.
  const char *raw_ptr = properties.ptr();
  const int64_t raw_len = properties.length();

  // The reformatted property json only depends on the parser name and on the
  // raw property json, so it is taken from the cache when possible.
  {
    common::TCRLockGuard guard(get_formatted_props_cache_lock());
    ObFTFormattedPropsEntry *cache = get_formatted_props_cache();
    for (int64_t i = 0; i < MAX_FORMATTED_PROPS_CACHE_CNT; ++i) {
      if (cache[i].is_used_ && is_formatted_props_key_match(cache[i], parser_name_, properties)) {
        ret = copy_to_allocator(ObString(cache[i].formatted_len_, cache[i].formatted_), alloc, properties_);
        cache_hit = OB_SUCC(ret);
        break;
      }
    }
  }

  if (OB_SUCC(ret) && cache_hit) {
    // pass
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(alloc, properties_))) {
  } else {
    // store the result in the cache for the next calls
    common::TCWLockGuard guard(get_formatted_props_cache_lock());
    ObFTFormattedPropsEntry *cache = get_formatted_props_cache();
    ObFTFormattedPropsEntry *slot = nullptr;
    for (int64_t i = 0; i < MAX_FORMATTED_PROPS_CACHE_CNT; ++i) {
      if (!cache[i].is_used_) {
        slot = &cache[i];
        break;
      }
    }
    if (nullptr != slot) {
      common::ObArenaAllocator &cache_alloc = get_formatted_props_cache_alloc();
      char *name_buf = static_cast<char *>(cache_alloc.alloc(parser_name_.length() + 1));
      char *raw_buf = static_cast<char *>(cache_alloc.alloc(raw_len + 1));
      char *fmt_buf = static_cast<char *>(cache_alloc.alloc(properties_.length() + 1));
      if (nullptr != name_buf && nullptr != raw_buf && nullptr != fmt_buf) {
        if (parser_name_.length() > 0) {
          MEMCPY(name_buf, parser_name_.ptr(), parser_name_.length());
        }
        name_buf[parser_name_.length()] = '\0';
        if (raw_len > 0) {
          MEMCPY(raw_buf, raw_ptr, raw_len);
        }
        raw_buf[raw_len] = '\0';
        if (properties_.length() > 0) {
          MEMCPY(fmt_buf, properties_.ptr(), properties_.length());
        }
        fmt_buf[properties_.length()] = '\0';
        slot->parser_name_ = name_buf;
        slot->parser_name_len_ = parser_name_.length();
        slot->raw_ = raw_buf;
        slot->raw_len_ = raw_len;
        slot->formatted_ = fmt_buf;
        slot->formatted_len_ = properties_.length();
        slot->is_used_ = true;
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
