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
  return (this == &other)
         || (word_len == other.word_len && 0 == memcmp(word, other.word, word_len));
}

int32_t ObFTSingleWord::set_word(const char *word, int32_t word_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(word)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (word_len > ObCharset::MAX_MB_LEN) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    memcpy(this->word, word, word_len);
    // Zero the rest of the buffer so that the word has exactly one
    // representation.  The hash of the dictionary map is computed over the
    // whole fixed size buffer, so a stale byte behind the word would make
    // the hash of the same word depend on the previous content of the
    // buffer.
    if (word_len < ObCharset::MAX_MB_LEN) {
      memset(this->word + word_len, 0, ObCharset::MAX_MB_LEN - word_len);
    }
    this->word_len = word_len;
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
