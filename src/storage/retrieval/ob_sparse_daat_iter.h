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

#ifndef OB_SPARSE_DAAT_ITER_H_
#define OB_SPARSE_DAAT_ITER_H_

#include "ob_i_sparse_retrieval_iter.h"
#include "ob_sparse_utils.h"
#include "storage/access/ob_scan_merge_loser_tree.h"
#include "storage/access/ob_simple_rows_merger.h"

namespace oceanbase
{
namespace storage
{

struct ObSRMergeItem
{
  ObSRMergeItem() : relevance_(0.0), iter_idx_(-1), equal_with_next_(false), id_datum_(nullptr) {}
  ~ObSRMergeItem() = default;
  TO_STRING_KV(K_(iter_idx), K_(relevance), K_(equal_with_next));

  double relevance_;
  int64_t iter_idx_;
  bool equal_with_next_;
  // Doc id datum captured when the item was pushed.  The dimension iterators
  // only advance inside fill_merge_heap(), so the datum stays valid until the
  // item is popped, and the merge comparator avoids the iter_ids_ indirection.
  const ObDatum *id_datum_;
};

struct ObSRMergeCmp
{
  ObSRMergeCmp();
  virtual ~ObSRMergeCmp() {}

  int init(sql::ObDatumMeta id_meta, const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids);
  inline int cmp(const ObSRMergeItem &l, const ObSRMergeItem &r, int64_t &cmp_ret)
  {
    int ret = OB_SUCCESS;
    if (IS_NOT_INIT) {
      ret = OB_NOT_INIT;
    } else {
      int tmp_ret = 0;
      const ObDatum &l_id = (OB_LIKELY(nullptr != l.id_datum_)) ? *l.id_datum_ : get_id_datum(l.iter_idx_);
      const ObDatum &r_id = (OB_LIKELY(nullptr != r.id_datum_)) ? *r.id_datum_ : get_id_datum(r.iter_idx_);
      if (OB_LIKELY(fast_bin_string_) && OB_LIKELY(!l_id.is_null() && !r_id.is_null())) {
        cmp_ret = bin_string_cmp(l_id, r_id);
      } else if (OB_FAIL(cmp_func_(l_id, r_id, tmp_ret, nullptr))) {
      } else {
        cmp_ret = tmp_ret;
      }
    }
    return ret;
  }
private:
  inline const ObDatum &get_id_datum(const int64_t iter_idx)
  {
    const ObDatum *datum = iter_ids_->get_data()[iter_idx];
    OB_ASSERT(nullptr != datum);
    return *datum;
  }
  inline static int64_t bin_string_cmp(const ObDatum &left, const ObDatum &right)
  {
    const int64_t min_len = MIN(left.len_, right.len_);
    const int mem_cmp = (min_len > 0) ? memcmp(left.ptr_, right.ptr_, static_cast<size_t>(min_len)) : 0;
    return (0 == mem_cmp) ? (static_cast<int64_t>(left.len_) - static_cast<int64_t>(right.len_)) : mem_cmp;
  }
private:
  common::ObDatumCmpFuncType cmp_func_;
  // TODO: if memory lifetime of docid datum is guaranteed by dim_iters, we can use pointer to datum directly
  //       and avoid deep copy into merge heap here
  const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids_;
  bool is_inited_;
  // The doc id is a binary string, so comparing two ids of the same length is
  // a plain memcmp. The merge heap compares ids on every output row, so this
  // fast path avoids the generic collation comparison.
  bool fast_bin_string_;
};

typedef ObSimpleRowsMerger<ObSRMergeItem, ObSRMergeCmp> ObSRSimpleMerger;
typedef ObMergeLoserTree<ObSRMergeItem, ObSRMergeCmp> ObSRLoserTree;
typedef common::ObRowsMerger<ObSRMergeItem, ObSRMergeCmp> ObSRMergeHeap;

// implementation of basic DaaT query processing algorithm primitives
class ObSRDaaTIterImpl : public ObISparseRetrievalMergeIter
{
public:
  ObSRDaaTIterImpl();
  virtual ~ObSRDaaTIterImpl() {}
  virtual int get_next_row() override;
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  int init(
      ObSparseRetrievalMergeParam &iter_param,
      ObIArray<ObISRDaaTDimIter *> &dim_iters,
      ObIAllocator &iter_allocator,
      ObSRDaaTRelevanceCollector &relevance_collector);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  virtual int get_query_max_score(double &score) override;
  
  INHERIT_TO_STRING_KV("ObISparseRetrievalMergeIter", ObISparseRetrievalMergeIter,
      K_(next_round_iter_idxes), K_(next_round_cnt));
protected:
  virtual int pre_process();
  virtual int do_one_merge_round(int64_t &count);
  virtual int fill_merge_heap();
  virtual int collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id);
  virtual int process_collected_row(const ObDatum &id_datum, const double relevance);
  virtual int filter_on_demand(const int64_t count, const double relevance, bool &need_project);
  virtual int cache_result(int64_t &count, const ObDatum &id_datum, const double relevance);
  virtual int project_results(const int64_t count);
  int init_merge_heap(const int64_t count);
  bool merge_heap_empty() const
  {
    return OB_LIKELY(nullptr != simple_merge_heap_) ? simple_merge_heap_->empty() : merge_heap_->empty();
  }
  bool merge_heap_is_unique_champion() const
  {
    return OB_LIKELY(nullptr != simple_merge_heap_)
        ? simple_merge_heap_->is_unique_champion() : merge_heap_->is_unique_champion();
  }
  int merge_heap_push(const ObSRMergeItem &item)
  {
    return OB_LIKELY(nullptr != simple_merge_heap_) ? simple_merge_heap_->push(item) : merge_heap_->push(item);
  }
  int merge_heap_top(const ObSRMergeItem *&item)
  {
    return OB_LIKELY(nullptr != simple_merge_heap_) ? simple_merge_heap_->top(item) : merge_heap_->top(item);
  }
  int merge_heap_pop()
  {
    return OB_LIKELY(nullptr != simple_merge_heap_) ? simple_merge_heap_->pop() : merge_heap_->pop();
  }
  int merge_heap_rebuild()
  {
    return OB_LIKELY(nullptr != simple_merge_heap_) ? simple_merge_heap_->rebuild() : merge_heap_->rebuild();
  }
  int collect_relevance(const int64_t dim_idx, const double relevance)
  {
    return OB_LIKELY(nullptr != inner_product_collector_)
        ? inner_product_collector_->collect_one_dim(dim_idx, relevance)
        : relevance_collector_->collect_one_dim(dim_idx, relevance);
  }
  int get_relevance_result(double &relevance, bool &is_valid)
  {
    return OB_LIKELY(nullptr != inner_product_collector_)
        ? inner_product_collector_->get_result(relevance, is_valid)
        : relevance_collector_->get_result(relevance, is_valid);
  }
protected:
  ObIAllocator *iter_allocator_;
  ObSparseRetrievalMergeParam *iter_param_;
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSRMergeCmp merge_cmp_;
  ObSRMergeHeap *merge_heap_;
  ObSRSimpleMerger *simple_merge_heap_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObSRDaaTInnerProductRelevanceCollector *inner_product_collector_;
  ObFixedArray<const ObDatum *, ObIAllocator> iter_domain_ids_; // record every dim iter's output domain id, one (ObDatum *) for one dim iter
  ObFixedArray<ObDocIdExt, ObIAllocator> buffered_domain_ids_; // cache for output
  ObFixedArray<double, ObIAllocator> buffered_relevances_;
  ObFixedArray<int64_t, ObIAllocator> next_round_iter_idxes_;
  int64_t next_round_cnt_;
  void (*set_datum_func_)(ObDatum &, const ObDocIdExt &);
  // Raw data pointers of the fixed arrays above and of dim_iters_, cached in
  // init().  Every array is allocated once by prepare_allocate() and is never
  // reallocated before reset(), so the virtual get_data() dispatch can be
  // replaced with a plain load on the merge hot path.  reset() clears them.
  const ObDatum **iter_ids_data_;
  ObDocIdExt *buffered_ids_data_;
  double *buffered_rel_data_;
  int64_t *next_round_idx_data_;
  ObISRDaaTDimIter **dim_iters_data_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObSRDaaTIterImpl);
};

} // namespace storage
} // namespace oceanbase

#endif
