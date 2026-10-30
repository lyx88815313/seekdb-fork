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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
namespace
{
// Cache of the dictionary codes of the character that is being matched.
//
// The active hits of the CJK processor can live in different dictionaries,
// and every dictionary has its own code space, so the code of a character
// has to be looked up per dictionary.  The hits usually all live in the same
// dictionary, so a small cache of the dictionaries that were seen already
// avoids looking the same character up again for every single hit.
class ObFTCJKCodeCache
{
public:
  static const int64_t MAX_DICT_CNT = 4;

public:
  ObFTCJKCodeCache() : dict_cnt_(0) {}

public:
  // Look the code of the word up in the given dictionary, reusing the code
  // of a previous lookup in the same dictionary if there is one.
  //
  // found is set to false when the word is not part of the dictionary.
  int get(const ObIFTDict *dict,
          const ObString &word,
          ObFTWordCode &code,
          bool &found)
  {
    int ret = OB_SUCCESS;
    found = false;
    bool cached = false;
    for (int64_t i = 0; i < dict_cnt_; ++i) {
      if (dicts_[i] == dict) {
        code = codes_[i];
        found = found_[i];
        cached = true;
        break;
      }
    }
    if (!cached) {
      ret = dict->find_word_code(word, code);
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
        found = false;
      } else if (OB_SUCC(ret)) {
        found = true;
      }
      if (OB_SUCC(ret) && dict_cnt_ < MAX_DICT_CNT) {
        dicts_[dict_cnt_] = dict;
        codes_[dict_cnt_] = code;
        found_[dict_cnt_] = found;
        ++dict_cnt_;
      }
    }
    return ret;
  }

private:
  const ObIFTDict *dicts_[MAX_DICT_CNT];
  ObFTWordCode codes_[MAX_DICT_CNT];
  bool found_[MAX_DICT_CNT];
  int64_t dict_cnt_;
};
} // anonymous namespace

int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    const ObString word(char_len, ch);
    // The character is looked up in the dictionary of the hit, so the lookup
    // is done once per dictionary instead of once per hit.
    ObFTCJKCodeCache code_cache;
    // handle previous hits_ first and then check from this char
    for (ObList<ObDATrieHit, ObIAllocator>::iterator iter = hits_.begin();
         OB_SUCC(ret) && iter != hits_.end();
         iter++) {
      ObDATrieHit &hit = *iter;
      const ObIFTDict *dict = hit.dict_;
      ObFTWordCode code = 0;
      bool code_found = false;
      if (OB_FAIL(code_cache.get(dict, word, code, code_found))) {
        break;
      } else if (!code_found) {
        // The character is not part of the dictionary of this hit, so the hit
        // cannot match any more.
        hit.set_unmatch();
      } else if (OB_FAIL(dict->match_with_code(word, code, hit, hit))) {
        break;
      }
      if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else if (hit.is_prefix()) {
          // match will record the start_cursor
        }
      } else if (hit.is_prefix()) {
        // nothing
      } else if (hit.is_unmatch()) {
        hits_.erase(hit);
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match(word, hit))) {
      } else if (hit.is_match()) {
        // output token
        hits_.push_back(hit);
        ctx.add_token(ctx.fulltext(),
                      ctx.get_cursor(),
                      char_len,
                      1,
                      ObIKTokenType::IK_CHINESE_TOKEN);
      } else if (hit.is_prefix()) {
        hits_.push_back(hit);
      } else {
        // ignore mismatch
      }
    }
  } else {
    // stop previous match
    hits_.clear();
  }

  if (OB_SUCC(ret)) {
    if (ctx.is_last()) {
      hits_.clear();
    }
  } else {
    hits_.clear();
  }

  return ret;
}

} //  namespace storage
} //  namespace oceanbase
