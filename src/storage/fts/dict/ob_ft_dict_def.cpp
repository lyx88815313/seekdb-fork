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

#include "storage/fts/dict/ob_ft_dict_def.h"

namespace oceanbase
{
namespace storage
{

common::ObString ObFTSingleWord::get_word() const { return ObString(word_len, word); }
bool ObFTSingleWord::operator==(const ObFTSingleWord &other) const
{
  if (this == &other) {
    return true;
  }
  if (word_len != other.word_len) {
    return false;
  }
  // Compare the bytes of the word.
  //
  // The words are compared without a call into libc: the length is a
  // variable, so memcmp() would be a call for every comparison, and the
  // words are compared once per probe of the dictionary hash map, which is
  // probed once per character of the text that is tokenized.  The bytes
  // behind the word are masked out, so exactly the bytes of the word are
  // compared, like memcmp() would compare them.
  uint64_t lhs = 0;
  uint64_t rhs = 0;
  MEMCPY(&lhs, word, sizeof(word));
  MEMCPY(&rhs, other.word, sizeof(other.word));
  if (word_len < sizeof(word)) {
    const uint64_t mask = (static_cast<uint64_t>(1) << (8 * word_len)) - 1;
    lhs &= mask;
    rhs &= mask;
  }
  return lhs == rhs;
}

int32_t ObFTSingleWord::set_word(const char *word, int32_t word_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(word)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (word_len > ObCharset::MAX_MB_LEN) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    // Copy the word into the fixed size buffer and zero the rest of it.
    //
    // The copy is done without a call into libc: the length is a variable,
    // so memcpy() would be a call for every word that is set, and a word is
    // set for every character that is looked up in the dictionary.  The
    // bytes behind the word are zero, which makes the word have exactly one
    // representation.  The hash of the dictionary map is computed over the
    // whole fixed size buffer, so a stale byte behind the word would make
    // the hash of the same word depend on the previous content of the
    // buffer.
    uint64_t chunk = 0;
    for (int32_t i = 0; i < word_len; ++i) {
      chunk |= static_cast<uint64_t>(static_cast<uint8_t>(word[i])) << (8 * i);
    }
    MEMCPY(this->word, &chunk, sizeof(this->word));
    this->word_len = word_len;
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
