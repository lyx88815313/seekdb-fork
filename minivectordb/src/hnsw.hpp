#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mvdb {

// Lightweight HNSW (Hierarchical Navigable Small World) index over float32
// vectors of a fixed dimension, keyed by int64 labels.
//
// Contract highlights:
//   * Vectors are copied into the index, the caller may reuse its buffers.
//   * Labels are unique: set() on an existing label replaces its vector.
//   * remove() tombstones the label; the node stays inside the graph (so the
//     topology does not degrade) but is never reported by search().
//   * search() is const, but it is not safe to run concurrently on the same
//     object (the index keeps a small amount of internal scratch state).
class HnswIndex {
 public:
  // dim: vector dimension (>0). max_elements: initial capacity hint.
  explicit HnswIndex(int dim, int max_elements = 1024);

  // Insert or replace the vector for `label`. Vectors are float32, length == dim().
  void set(int64_t label, const float* vec);

  // Remove `label`. Removing a non-existent label is a no-op.
  void remove(int64_t label);

  // Search the `k` nearest labels to `query` (length dim()).
  // Results are appended to `out` as (label, distance) pairs sorted by distance ascending,
  // then by label ascending for equal distances. Distance is the true L2 distance
  // (NOT squared). At most k results; fewer if fewer labels exist.
  // `ef` is the size of the candidate list of the graph traversal: a larger
  // value finds closer neighbours but costs more time. Zero selects the
  // built-in default.
  void search(const float* query, int k, std::vector<std::pair<int64_t, float>>& out,
              int ef = 0) const;

  // Number of live labels currently stored.
  std::size_t size() const;

  // Vector dimension.
  int dim() const;

  // Grow the internal storage to hold at least `n` elements.
  void reserve(std::size_t n);

  // Rebuild the whole graph from scratch from the given live vectors (used at startup
  // to rebuild the index from persisted rows). `labels` and `vectors` must have the
  // same length; vectors[i] points to dim() float32 values. Duplicated labels are
  // resolved by keeping the LAST occurrence.
  void rebuild(const std::vector<int64_t>& labels,
               const std::vector<const float*>& vectors);

 private:
  struct Cand {
    float dist;   // squared L2 distance
    uint32_t id;
  };

  // Ordering by distance ascending (the label is only used for the final
  // output ordering, which is applied after the candidates are collected).
  struct CandMinCmp {
    bool operator()(const Cand& a, const Cand& b) const;
  };
  struct CandMaxCmp {
    bool operator()(const Cand& a, const Cand& b) const;
  };

  static bool better(const Cand& a, const Cand& b);
  static bool worse(const Cand& a, const Cand& b);
  static bool ascending(const Cand& a, const Cand& b);

  static std::size_t level_base(int level);
  static int level_capacity(int level);
  static std::size_t level_slots(int level);

  const float* vec_ptr(uint32_t id) const;
  const uint32_t* nbr_begin(uint32_t id, int level) const;
  uint32_t* nbr_begin(uint32_t id, int level);
  float* nbr_dist_begin(uint32_t id, int level);
  uint8_t nbr_count(uint32_t id, int level) const;
  uint8_t& nbr_count_ref(uint32_t id, int level);

  void ensure_node_slots(std::size_t n);
  uint32_t add_node(int64_t label, const float* vec, int level);
  void mark_deleted(uint32_t id);
  void insert_new(int64_t label, const float* vec);
  int random_level();

  uint32_t greedy_descend(uint32_t entry, int level, const float* query) const;
  void search_layer(const float* query, int level, int ef, uint32_t seed,
                    std::vector<Cand>& out) const;
  std::vector<uint32_t> select_neighbors(const std::vector<Cand>& cands, int limit) const;
  void link_nodes(uint32_t owner, uint32_t other, int level);
  void prune_list(uint32_t owner, int level, uint32_t extra);
  void brute_force(const float* query, std::size_t need,
                   std::vector<std::pair<int64_t, float>>& out) const;
  uint32_t begin_epoch() const;
  void clear_state();

  int dim_;
  double level_mult_;
  std::size_t node_count_;
  std::size_t live_count_;
  std::size_t deleted_count_;

  std::vector<float> data_;            // node_count_ * dim_ floats, contiguous
  std::vector<int64_t> labels_;        // node id -> label
  std::vector<uint8_t> deleted_;       // node id -> tombstone flag
  std::vector<uint16_t> levels_;       // node id -> top level of the node
  std::vector<uint32_t> link_offset_;  // node id -> base offset into neighbors_
  std::vector<uint32_t> neighbors_;    // flat neighbour storage
  std::vector<float> nbr_dists_;       // distance owner -> neighbour (same layout)
  std::vector<uint8_t> nbr_counts_;    // flat neighbour counters (parallel layout)
  std::vector<uint32_t> live_nodes_;   // ids of the live nodes
  std::vector<uint32_t> live_pos_;     // node id -> position inside live_nodes_
  std::unordered_map<int64_t, uint32_t> label_to_node_;

  uint32_t entry_point_;
  int max_level_;

  std::mt19937 rng_;

  mutable std::vector<uint32_t> visit_stamp_;
  mutable uint32_t visit_epoch_;
};

}  // namespace mvdb
