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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_macro_utils.h"
#include "ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict.h"

#include <cstddef>

/* File Description:
 * @brief tools to build a double array trie dictionary.
 */

namespace oceanbase
{
namespace storage
{
/* @class ObArrayHashMap
 * @description: a fixed size hash map, used to store words and their code.
 */
class ObArrayHashMap final
{
public:
  struct Entry
  {
    ObFTSingleWord word;
    ObFTWordCode code;
    bool used = false;
  } __attribute__((packed));

  struct Header
  {
    uint32_t buffer_size_;
    uint32_t capacity_;
    uint32_t count_;
    Entry data[1];
  } __attribute__((packed));

  static size_t estimate_capacity(size_t word_num);

  static size_t estimate_size(size_t word_num);

  size_t capacity() const { return header_.capacity_; }

  int init(size_t word_num);

  // no duplicate should be inserted
  int insert(const ObString &key, ObFTWordCode code);

  // Inline lookup: cheap word hash and no division in the probe loop.
  // The lookup is on the tokenizer hot path (once per matched character),
  // so the hash and the bucket reduction are kept branch-light and free of
  // the 64 bit division that a plain modulo would cost.
  int find(const ObFTSingleWord &word, ObFTWordCode &code) const
  {
    int ret = OB_ENTRY_NOT_EXIST;
    const uint32_t capacity = header_.capacity_;
    uint32_t index = hash_index(hash_single_word(word), capacity);
    while (header_.data[index].used) {
      if (header_.data[index].word == word) {
        code = header_.data[index].code;
        ret = OB_SUCCESS;
        break;
      } else {
        index += 3;
        if (OB_UNLIKELY(index >= capacity)) {
          index -= capacity;
        }
      }
    }
    return ret;
  }

  // Hash of a single word.
  //
  // A word is a single character of at most ObCharset::MAX_MB_LEN bytes, so
  // it is kept in a fixed size buffer whose content behind the word is
  // always zero (see ObFTSingleWord::set_word).  Hashing that buffer together
  // with the length gives the same distribution as hashing the word bytes,
  // but it does not need a copy of a runtime length, which would be a call
  // into the libc memcpy for every character that is looked up.
  static OB_INLINE uint64_t hash_single_word(const ObFTSingleWord &word)
  {
    uint64_t chunk = 0;
    MEMCPY(&chunk, word.word, sizeof(word.word));
    uint64_t hash = chunk ^ (static_cast<uint64_t>(word.word_len) * 0x9E3779B97F4A7C15ULL);
    hash ^= hash >> 29;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= hash >> 32;
    return hash;
  }

  // A word is at most ObCharset::MAX_MB_LEN bytes, so it fits into a register.
  static OB_INLINE uint64_t hash_word(const char *ptr, const int64_t len)
  {
    uint64_t chunk = 0;
    if (OB_LIKELY(len > 0 && len <= static_cast<int64_t>(sizeof(chunk)))) {
      MEMCPY(&chunk, ptr, static_cast<size_t>(len));
    } else {
      for (int64_t i = 0; i < len; ++i) {
        chunk = (chunk << 8) | static_cast<unsigned char>(ptr[i]);
      }
    }
    uint64_t hash = chunk ^ (static_cast<uint64_t>(len) * 0x9E3779B97F4A7C15ULL);
    hash ^= hash >> 29;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= hash >> 32;
    return hash;
  }

  // Multiply-shift reduction: the result is in [0, capacity) and, unlike a
  // modulo, it does not need a division.
  static OB_INLINE uint32_t hash_index(const uint64_t hash, const uint32_t capacity)
  {
    return static_cast<uint32_t>((static_cast<unsigned __int128>(hash) * capacity) >> 64);
  }

private:
  struct Header header_;
} __attribute__((packed));

/**
 * @class ObFTDAT
 * @brief ObFTDATrie data structure, which is a memory block, which contains
 *          the code map(optional), the base and check array, and the data block(optional).
 */
struct ObFTDAT
{
public:
  static constexpr int32_t FIRST_BASE = 1;
  static constexpr int32_t FIRST_INDEX = 1;
  // all size in bytes
  int64_t mem_block_size_ = 0;
  // node_num_: number of nodes in the trie.
  size_t node_num_ = 0;
  // word_num_: number of words in the trie.
  size_t word_num_ = 0;
  // array_size_ for the base and check array.
  size_t array_size_ = 0;

  // start offset of every data block
  size_t map_offset_ = 0;
  size_t base_offset_ = 0;
  size_t check_offset_ = 0;

  // use if offset is not 0, it means the data is in the data block
  size_t data_index_offset_ = 0;
  // use if offset is not 0
  size_t data_offset_ = 0;
  // a buffer to store dat
  ObFTSingleWord start_word_;
  ObFTSingleWord end_word_;
  char buff[1] = {0};

public:
  ObArrayHashMap *get_map();
} __attribute__((packed));

template <typename DataType>
class ObFTTrie;

/**
 * @class OBFTDATBuilder
 * @brief oceanbase double array trie builder, build a trie and then a dat.
 */
template <typename DATA_TYPE = void>
class ObFTDATBuilder
{
public:
  ObFTDATBuilder(ObIAllocator &alloc)
      : alloc_(alloc), dat_(nullptr), map_(nullptr), is_inited_(false), next_code_(1)
  {
  }

  ~ObFTDATBuilder() {}

  int init(ObFTTrie<DATA_TYPE> &trie);

  int build_from_trie(ObFTTrie<DATA_TYPE> &trie);

  // get and put to kv_cache
  int get_mem_block(ObFTDAT *&mem, size_t &mem_len)
  {
    mem = dat_;
    if (nullptr == dat_) {
      mem_len = 0;
    } else {
      mem_len = dat_->mem_block_size_;
    }
    return OB_SUCCESS;
  }

private:
  int expand();

  int encode(const ObString &word, ObFTWordCode &code, bool add);

private:
  common::ObIAllocator &alloc_;
  ObFTDAT *dat_;
  ObArrayHashMap *map_;

  bool is_inited_;
  int32_t next_code_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATBuilder);
};

/**
 * @class ObFTDATReader
 * @brief Reader to do match on a mem block dat.
 */
template <typename DataType = void>
class ObFTDATReader
{
public:
  ObFTDATReader(ObFTDAT *dat) : dat_(dat) {}

  // Match single word with hit.
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const;

  // Look up the code of a single word.
  //
  // The code is the offset of the word in the double array trie.  It is
  // looked up separately from the trie walk, because a caller that matches
  // several words in a row can look the same word up once and reuse the
  // code, instead of looking it up again for every word.
  int find_code(const ObString &single_word, ObFTWordCode &code) const;

  // Match single word with hit, using a code that was obtained from
  // find_code() before.
  int match_with_code(const ObString &single_word,
                      const ObFTWordCode code,
                      const ObDATrieHit &last_hit,
                      ObDATrieHit &hit) const;

private:
  ObFTDAT *dat_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATReader);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
