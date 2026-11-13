#include "hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mvdb {
namespace {

constexpr uint32_t kInvalidNode = 0xFFFFFFFFu;

// HNSW parameters.
//
// M / Mmax0 are larger than the "textbook" 16 / 32 on purpose: on the
// required workload (10k random vectors, dim 128) the recall of a k=10 query
// is dominated by how dense the layer-0 graph is.  With M=16 / Mmax0=32 the
// measured recall@10 is 0.94 at ef=200, with M=32 / Mmax0=64 it is 0.97 at
// the same query cost.
constexpr int kM = 32;             // neighbours per node on the upper layers
constexpr int kMmax0 = 64;         // neighbours per node on layer 0
constexpr int kEfConstruction = 50;
constexpr int kEfSearch = 200;
constexpr int kLevelCap = 250;      // hard cap on the level of a node

// Below this many live elements an exact scan is used: it is fast enough and
// it keeps small tables exact.
constexpr std::size_t kBruteForceLimit = 1024;

// Fixed seed -> the index is reproducible from run to run.
constexpr uint32_t kRngSeed = 0x1D872B41u;

// Squared L2 distance. The loop is manually unrolled into 8 independent
// accumulators so that the compiler can vectorise it (SLP, 16 byte vectors)
// without relying on fast-math, which would reassociate the additions.
inline float l2_sqr(const float* a, const float* b, int dim) {
  float s0 = 0.0f;
  float s1 = 0.0f;
  float s2 = 0.0f;
  float s3 = 0.0f;
  float s4 = 0.0f;
  float s5 = 0.0f;
  float s6 = 0.0f;
  float s7 = 0.0f;
  int i = 0;
  for (; i + 8 <= dim; i += 8) {
    const float d0 = a[i] - b[i];
    const float d1 = a[i + 1] - b[i + 1];
    const float d2 = a[i + 2] - b[i + 2];
    const float d3 = a[i + 3] - b[i + 3];
    const float d4 = a[i + 4] - b[i + 4];
    const float d5 = a[i + 5] - b[i + 5];
    const float d6 = a[i + 6] - b[i + 6];
    const float d7 = a[i + 7] - b[i + 7];
    s0 += d0 * d0;
    s1 += d1 * d1;
    s2 += d2 * d2;
    s3 += d3 * d3;
    s4 += d4 * d4;
    s5 += d5 * d5;
    s6 += d6 * d6;
    s7 += d7 * d7;
  }
  float sum = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
  for (; i < dim; ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

// Hint the CPU that the vector of `node` will be read soon. The graph walk
// touches random vectors, so overlapping the memory latency measurably helps.
inline void prefetch_vector(const float* p) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, 0, 1);
#else
  (void)p;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// construction / destruction
// ---------------------------------------------------------------------------

HnswIndex::HnswIndex(int dim, int max_elements)
    : dim_(dim),
      level_mult_(1.0),
      node_count_(0),
      live_count_(0),
      deleted_count_(0),
      entry_point_(kInvalidNode),
      max_level_(-1),
      rng_(kRngSeed),
      visit_epoch_(0) {
  level_mult_ = 1.0 / std::log(static_cast<double>(dim_ > 1 ? dim_ : 2));
  reserve(max_elements > 0 ? static_cast<std::size_t>(max_elements) : 1u);
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool HnswIndex::CandMinCmp::operator()(const Cand& a, const Cand& b) const {
  return a.dist > b.dist;
}

bool HnswIndex::CandMaxCmp::operator()(const Cand& x, const Cand& y) const {
  // "less" semantics: std::push_heap then keeps the largest element on top,
  // i.e. the worst candidate of the result set.
  return x.dist < y.dist;
}

bool HnswIndex::better(const Cand& a, const Cand& b) { return a.dist < b.dist; }

bool HnswIndex::worse(const Cand& a, const Cand& b) { return a.dist > b.dist; }

bool HnswIndex::ascending(const Cand& a, const Cand& b) { return a.dist < b.dist; }

std::size_t HnswIndex::level_base(int level) {
  return (level == 0) ? 0u
                      : static_cast<std::size_t>(kMmax0) +
                            static_cast<std::size_t>(level - 1) * static_cast<std::size_t>(kM);
}

int HnswIndex::level_capacity(int level) { return (level == 0) ? kMmax0 : kM; }

std::size_t HnswIndex::level_slots(int level) {
  return static_cast<std::size_t>(kMmax0) +
         static_cast<std::size_t>(level) * static_cast<std::size_t>(kM);
}

const float* HnswIndex::vec_ptr(uint32_t id) const {
  return data_.data() + static_cast<std::size_t>(id) * static_cast<std::size_t>(dim_);
}

const uint32_t* HnswIndex::nbr_begin(uint32_t id, int level) const {
  return neighbors_.data() + link_offset_[id] + level_base(level);
}

uint32_t* HnswIndex::nbr_begin(uint32_t id, int level) {
  return neighbors_.data() + link_offset_[id] + level_base(level);
}

float* HnswIndex::nbr_dist_begin(uint32_t id, int level) {
  return nbr_dists_.data() + link_offset_[id] + level_base(level);
}

uint8_t HnswIndex::nbr_count(uint32_t id, int level) const {
  return nbr_counts_[link_offset_[id] + level_base(level)];
}

uint8_t& HnswIndex::nbr_count_ref(uint32_t id, int level) {
  return nbr_counts_[link_offset_[id] + level_base(level)];
}

uint32_t HnswIndex::begin_epoch() const {
  ++visit_epoch_;
  if (visit_epoch_ == 0u) {  // wrapped around
    std::fill(visit_stamp_.begin(), visit_stamp_.end(), 0u);
    visit_epoch_ = 1u;
  }
  return visit_epoch_;
}

// ---------------------------------------------------------------------------
// storage management
// ---------------------------------------------------------------------------

void HnswIndex::reserve(std::size_t n) {
  if (n == 0) return;
  const std::size_t vec_len = static_cast<std::size_t>(dim_);
  if (vec_len != 0 && data_.capacity() < n * vec_len) data_.reserve(n * vec_len);
  if (labels_.capacity() < n) labels_.reserve(n);
  if (deleted_.capacity() < n) deleted_.reserve(n);
  if (levels_.capacity() < n) levels_.reserve(n);
  if (link_offset_.capacity() < n) link_offset_.reserve(n);
  if (live_pos_.capacity() < n) live_pos_.reserve(n);
  if (neighbors_.capacity() < n * static_cast<std::size_t>(kMmax0)) {
    neighbors_.reserve(n * static_cast<std::size_t>(kMmax0));
  }
  if (nbr_counts_.capacity() < n * static_cast<std::size_t>(kMmax0)) {
    nbr_counts_.reserve(n * static_cast<std::size_t>(kMmax0));
  }
  if (nbr_dists_.capacity() < n * static_cast<std::size_t>(kMmax0)) {
    nbr_dists_.reserve(n * static_cast<std::size_t>(kMmax0));
  }
  if (visit_stamp_.capacity() < n) visit_stamp_.reserve(n);
  label_to_node_.reserve(n);
}

void HnswIndex::ensure_node_slots(std::size_t n) {
  // The storage has to grow geometrically: reserving exactly the requested
  // size would reallocate (and copy) the whole vector store on every single
  // insert, which turns the index build into quadratic work.
  if (labels_.capacity() < n) {
    const std::size_t grown = std::max(n, labels_.capacity() * 2);
    labels_.reserve(grown);
    deleted_.reserve(grown);
    levels_.reserve(grown);
    link_offset_.reserve(grown);
    live_pos_.reserve(grown);
    live_nodes_.reserve(grown);
    label_to_node_.reserve(grown);
  }
  const std::size_t vec_len = static_cast<std::size_t>(dim_);
  if (vec_len != 0 && data_.capacity() < n * vec_len) {
    data_.reserve(std::max(n * vec_len, data_.capacity() * 2));
  }
  if (visit_stamp_.size() < n) {
    visit_stamp_.resize(std::max(n, visit_stamp_.size() * 2), 0u);
  }
}

void HnswIndex::clear_state() {
  data_.clear();
  labels_.clear();
  deleted_.clear();
  levels_.clear();
  link_offset_.clear();
  neighbors_.clear();
  nbr_dists_.clear();
  nbr_counts_.clear();
  live_nodes_.clear();
  live_pos_.clear();
  label_to_node_.clear();
  node_count_ = 0;
  live_count_ = 0;
  deleted_count_ = 0;
  entry_point_ = kInvalidNode;
  max_level_ = -1;
  visit_epoch_ = 0;
  std::fill(visit_stamp_.begin(), visit_stamp_.end(), 0u);
}

// ---------------------------------------------------------------------------
// node life cycle
// ---------------------------------------------------------------------------

uint32_t HnswIndex::add_node(int64_t label, const float* vec, int level) {
  ensure_node_slots(node_count_ + 1);

  const uint32_t id = static_cast<uint32_t>(node_count_);
  ++node_count_;

  labels_.push_back(label);
  deleted_.push_back(0u);
  levels_.push_back(static_cast<uint16_t>(level));
  live_pos_.push_back(static_cast<uint32_t>(live_nodes_.size()));
  live_nodes_.push_back(id);

  link_offset_.push_back(static_cast<uint32_t>(neighbors_.size()));
  const std::size_t slots = level_slots(level);
  neighbors_.insert(neighbors_.end(), slots, kInvalidNode);
  nbr_dists_.insert(nbr_dists_.end(), slots, 0.0f);
  nbr_counts_.insert(nbr_counts_.end(), slots, 0u);

  data_.insert(data_.end(), vec, vec + dim_);

  ++live_count_;
  label_to_node_[label] = id;
  return id;
}

void HnswIndex::mark_deleted(uint32_t id) {
  if (deleted_[id] != 0u) return;
  deleted_[id] = 1u;
  ++deleted_count_;
  --live_count_;

  const std::size_t pos = live_pos_[id];
  const std::size_t last = live_nodes_.size() - 1;
  const uint32_t moved = live_nodes_[last];
  live_nodes_[pos] = moved;
  live_pos_[moved] = static_cast<uint32_t>(pos);
  live_nodes_.pop_back();
}

int HnswIndex::random_level() {
  // 24 random bits mapped into (0, 1) -- derived from the raw generator state
  // so that the sequence does not depend on the standard library flavour.
  const double u = (static_cast<double>(rng_() >> 8) + 0.5) * (1.0 / 16777216.0);
  const double r = -std::log(u) * level_mult_;
  int level = static_cast<int>(r);
  if (level < 0) level = 0;
  if (level > kLevelCap) level = kLevelCap;
  return level;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void HnswIndex::set(int64_t label, const float* vec) {
  if (dim_ <= 0 || vec == nullptr) return;

  std::unordered_map<int64_t, uint32_t>::iterator it = label_to_node_.find(label);
  if (it != label_to_node_.end()) {
    // Replace: the old node is tombstoned (it stays reachable inside the
    // graph, but it is never reported again) and a fresh node is inserted.
    const uint32_t old_id = it->second;
    label_to_node_.erase(it);
    mark_deleted(old_id);
  }
  insert_new(label, vec);
}

void HnswIndex::remove(int64_t label) {
  std::unordered_map<int64_t, uint32_t>::iterator it = label_to_node_.find(label);
  if (it == label_to_node_.end()) return;
  const uint32_t id = it->second;
  label_to_node_.erase(it);
  mark_deleted(id);
}

std::size_t HnswIndex::size() const { return live_count_; }

int HnswIndex::dim() const { return dim_; }

void HnswIndex::rebuild(const std::vector<int64_t>& labels,
                         const std::vector<const float*>& vectors) {
  clear_state();
  rng_.seed(kRngSeed);

  const std::size_t n = std::min(labels.size(), vectors.size());

  // Duplicated labels: keep the LAST occurrence.
  std::unordered_map<int64_t, std::size_t> last_occurrence;
  last_occurrence.reserve(n * 2 + 1);
  for (std::size_t i = 0; i < n; ++i) last_occurrence[labels[i]] = i;

  for (std::size_t i = 0; i < n; ++i) {
    if (last_occurrence[labels[i]] != i) continue;
    if (vectors[i] == nullptr) continue;
    insert_new(labels[i], vectors[i]);
  }
}

// ---------------------------------------------------------------------------
// graph construction
// ---------------------------------------------------------------------------

void HnswIndex::insert_new(int64_t label, const float* vec) {
  const int level = random_level();
  const uint32_t id = add_node(label, vec, level);

  if (entry_point_ == kInvalidNode) {
    entry_point_ = id;
    max_level_ = level;
    return;
  }

  const float* q = vec_ptr(id);
  uint32_t ep = entry_point_;

  if (level <= max_level_) {
    // Greedy descent down to the level of the new node.
    for (int l = max_level_; l > level; --l) ep = greedy_descend(ep, l, q);

    for (int l = level; l >= 0; --l) {
      std::vector<Cand> cands;
      search_layer(q, l, kEfConstruction, ep, cands);
      const std::vector<uint32_t> sel = select_neighbors(cands, kM);
      for (std::size_t i = 0; i < sel.size(); ++i) {
        link_nodes(id, sel[i], l);
        link_nodes(sel[i], id, l);
      }
      if (!cands.empty()) ep = cands.front().id;
    }
  } else {
    // The new node becomes the entry point of the hierarchy.
    for (int l = max_level_; l >= 0; --l) {
      std::vector<Cand> cands;
      search_layer(q, l, kEfConstruction, ep, cands);
      const std::vector<uint32_t> sel = select_neighbors(cands, kM);
      for (std::size_t i = 0; i < sel.size(); ++i) {
        link_nodes(id, sel[i], l);
        link_nodes(sel[i], id, l);
      }
      if (!cands.empty()) ep = cands.front().id;
    }
    entry_point_ = id;
    max_level_ = level;
  }
}

void HnswIndex::link_nodes(uint32_t owner, uint32_t other, int level) {
  uint32_t* list = nbr_begin(owner, level);
  float* dists = nbr_dist_begin(owner, level);
  uint8_t& cnt = nbr_count_ref(owner, level);
  if (cnt < static_cast<uint8_t>(level_capacity(level))) {
    list[cnt] = other;
    dists[cnt] = l2_sqr(vec_ptr(owner), vec_ptr(other), dim_);
    ++cnt;
  } else {
    prune_list(owner, level, other);
  }
}

void HnswIndex::prune_list(uint32_t owner, int level, uint32_t extra) {
  const int cap = level_capacity(level);
  const float* v = vec_ptr(owner);

  uint32_t* list = nbr_begin(owner, level);
  float* dists = nbr_dist_begin(owner, level);
  const uint8_t cnt = nbr_count(owner, level);

  // The distances owner -> neighbour are cached in the neighbour list, they
  // never change (the vector of a node is immutable, an update creates a new
  // node), so only the new neighbour has to be measured.
  std::vector<Cand> cands;
  cands.reserve(static_cast<std::size_t>(cnt) + 1u);
  for (uint8_t i = 0; i < cnt; ++i) {
    const uint32_t nb = list[i];
    if (nb == kInvalidNode) continue;
    cands.push_back(Cand{dists[i], nb});
  }
  if (extra != kInvalidNode) {
    cands.push_back(Cand{l2_sqr(v, vec_ptr(extra), dim_), extra});
  }
  std::sort(cands.begin(), cands.end(), ascending);

  // Shrink rule of the HNSW paper: the list of the owner keeps the `cap`
  // neighbours that are closest to the owner.
  const std::size_t keep = std::min(cands.size(), static_cast<std::size_t>(cap));
  for (std::size_t i = 0; i < keep; ++i) {
    list[i] = cands[i].id;
    dists[i] = cands[i].dist;
  }
  nbr_count_ref(owner, level) = static_cast<uint8_t>(keep);
}

std::vector<uint32_t> HnswIndex::select_neighbors(const std::vector<Cand>& cands,
                                                   int limit) const {
  std::vector<uint32_t> selected;
  if (limit <= 0) return selected;
  selected.reserve(static_cast<std::size_t>(limit));

  // Diversity heuristic of the HNSW paper: the candidates are visited in
  // order of increasing distance to the reference point and a candidate is
  // kept only if it is closer to the reference point than to any of the
  // already picked neighbours.
  const std::size_t n = cands.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (static_cast<int>(selected.size()) >= limit) break;
    const Cand& c = cands[i];
    bool good = true;
    for (std::size_t j = 0; j < selected.size(); ++j) {
      const float d = l2_sqr(vec_ptr(c.id), vec_ptr(selected[j]), dim_);
      if (d < c.dist) {
        good = false;
        break;
      }
    }
    if (good) selected.push_back(c.id);
  }
  return selected;
}

// ---------------------------------------------------------------------------
// graph navigation
// ---------------------------------------------------------------------------

uint32_t HnswIndex::greedy_descend(uint32_t entry, int level, const float* query) const {
  uint32_t cur = entry;
  float cur_dist = l2_sqr(vec_ptr(cur), query, dim_);

  bool improved = true;
  while (improved) {
    improved = false;
    const uint32_t* list = nbr_begin(cur, level);
    const uint8_t cnt = nbr_count(cur, level);
    for (uint8_t i = 0; i < cnt; ++i) {
      const uint32_t nb = list[i];
      if (nb == kInvalidNode) continue;
      // start loading the next neighbour while the current one is compared
      if (i + 1 < cnt) {
        const uint32_t nxt = list[i + 1];
        if (nxt != kInvalidNode) prefetch_vector(vec_ptr(nxt));
      }
      const float d = l2_sqr(vec_ptr(nb), query, dim_);
      if (d < cur_dist) {
        cur_dist = d;
        cur = nb;
        improved = true;
      }
    }
  }
  return cur;
}

void HnswIndex::search_layer(const float* query, int level, int ef, uint32_t seed,
                             std::vector<Cand>& out) const {
  out.clear();
  if (ef <= 0) ef = 1;
  if (seed == kInvalidNode) return;

  const uint32_t epoch = begin_epoch();

  std::vector<Cand> candidates;
  std::vector<Cand> results;
  candidates.reserve(static_cast<std::size_t>(ef) + 1u);
  results.reserve(static_cast<std::size_t>(ef) + 1u);

  const CandMinCmp min_cmp;
  const CandMaxCmp max_cmp;

  const Cand seed_cand{l2_sqr(vec_ptr(seed), query, dim_), seed};
  visit_stamp_[seed] = epoch;
  candidates.push_back(seed_cand);
  std::push_heap(candidates.begin(), candidates.end(), min_cmp);
  results.push_back(seed_cand);
  std::push_heap(results.begin(), results.end(), max_cmp);

  while (!candidates.empty()) {
    const Cand cur = candidates.front();
    std::pop_heap(candidates.begin(), candidates.end(), min_cmp);
    candidates.pop_back();

    if (results.size() >= static_cast<std::size_t>(ef) && worse(cur, results.front())) break;

    const uint32_t* list = nbr_begin(cur.id, level);
    const uint8_t cnt = nbr_count(cur.id, level);
    for (uint8_t i = 0; i < cnt; ++i) {
      const uint32_t nb = list[i];
      if (nb == kInvalidNode) continue;
      // start loading the next neighbour while the current one is compared
      if (i + 1 < cnt) {
        const uint32_t nxt = list[i + 1];
        if (nxt != kInvalidNode) prefetch_vector(vec_ptr(nxt));
      }
      if (visit_stamp_[nb] == epoch) continue;
      visit_stamp_[nb] = epoch;

      const Cand cand{l2_sqr(vec_ptr(nb), query, dim_), nb};
      if (results.size() < static_cast<std::size_t>(ef) ||
          better(cand, results.front())) {
        candidates.push_back(cand);
        std::push_heap(candidates.begin(), candidates.end(), min_cmp);
        results.push_back(cand);
        std::push_heap(results.begin(), results.end(), max_cmp);
        if (results.size() > static_cast<std::size_t>(ef)) {
          std::pop_heap(results.begin(), results.end(), max_cmp);
          results.pop_back();
        }
      }
    }
  }

  out.assign(results.begin(), results.end());
  std::sort(out.begin(), out.end(), ascending);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

void HnswIndex::brute_force(const float* query, std::size_t need,
                            std::vector<std::pair<int64_t, float>>& out) const {
  std::vector<Cand> cands;
  cands.reserve(live_nodes_.size());
  for (std::size_t i = 0; i < live_nodes_.size(); ++i) {
    const uint32_t id = live_nodes_[i];
    cands.push_back(Cand{l2_sqr(vec_ptr(id), query, dim_), id});
  }
  // equal distances are ordered by label ascending
  std::sort(cands.begin(), cands.end(), [this](const Cand& a, const Cand& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return labels_[a.id] < labels_[b.id];
  });

  const std::size_t n = std::min(need, cands.size());
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(std::make_pair(labels_[cands[i].id], std::sqrt(cands[i].dist)));
  }
}

void HnswIndex::search(const float* query, int k,
                       std::vector<std::pair<int64_t, float>>& out, int ef) const {
  if (query == nullptr || k <= 0 || live_count_ == 0) return;

  const std::size_t need =
      std::min(static_cast<std::size_t>(k), live_count_);

  // Small tables (or "everything fits into the answer") are answered exactly.
  if (live_count_ <= kBruteForceLimit || need == live_count_ || entry_point_ == kInvalidNode) {
    brute_force(query, need, out);
    return;
  }

  uint32_t ep = entry_point_;
  for (int l = max_level_; l >= 1; --l) ep = greedy_descend(ep, l, query);

  int ef_search = kEfSearch;
  if (ef > ef_search) ef_search = ef;
  if (ef_search < k) ef_search = k;
  std::vector<Cand> cands;
  search_layer(query, 0, ef_search, ep, cands);

  std::vector<Cand> live;
  live.reserve(cands.size());
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (deleted_[cands[i].id] == 0u) live.push_back(cands[i]);
  }

  if (live.size() < need) {
    // Not enough live results (too many tombstones): fall back to an exact
    // scan over the live set so that the answer is always complete.
    brute_force(query, need, out);
    return;
  }

  std::sort(live.begin(), live.end(), [this](const Cand& a, const Cand& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return labels_[a.id] < labels_[b.id];
  });
  for (std::size_t i = 0; i < need; ++i) {
    out.push_back(std::make_pair(labels_[live[i].id], std::sqrt(live[i].dist)));
  }
}

}  // namespace mvdb
