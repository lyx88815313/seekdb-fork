// MiniVectorDB - table (row store) built on top of the page store.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.hpp"
#include "hnsw.hpp"
#include "sql.hpp"
#include "storage.hpp"

namespace mvdb {

// Where a row currently lives.
struct RowLocation {
  uint32_t page_id = 0;
  uint32_t slot_index = 0;
};

// A resolved comparison used by WHERE clauses.
struct Predicate {
  int column = -1;
  Compare::Op op = Compare::Op::Eq;
  int64_t integer = 0;
  std::string text;
  bool is_text = false;
};

// A decoded view over a row that is still inside a page buffer.
struct RowView {
  const uint8_t* data = nullptr;
  uint32_t length = 0;
  uint16_t null_mask = 0;
  int64_t pk = 0;
  uint32_t page_id = 0;
  uint32_t slot_index = 0;
};

// Undo record for the in-memory primary key and vector indexes. The indexes
// are derived structures, so they have to follow the same transaction
// boundaries as the persisted rows.
struct IndexUndo {
  int64_t pk = 0;
  bool had_location = false;
  RowLocation location;
  bool had_vector = false;
  std::vector<float> vector;
};

class Table {
 public:
  Table(Storage* storage, TableDef def);

  const TableDef& def() const { return def_; }
  int64_t id() const { return def_.id; }
  uint64_t row_count() const { return row_count_; }
  void set_row_count(uint64_t count) { row_count_ = count; }
  uint32_t page_count() const { return num_pages_; }
  void set_page_count(uint32_t count) { num_pages_ = count; }

  // Row decoding helpers.
  int64_t row_pk(const RowView& row) const { return row.pk; }
  int64_t row_int(const RowView& row, int column) const;
  std::string row_text(const RowView& row, int column) const;
  // Returns a pointer into the page buffer, avoiding a copy on the scan path.
  const char* row_text_view(const RowView& row, int column, uint32_t* length) const;
  const float* row_vector(const RowView& row) const;
  bool matches(const RowView& row, const std::vector<Predicate>& predicates) const;

  // Mutations. They throw DbException on invalid input and never leave a
  // partially applied statement behind.
  int64_t insert_row(const std::vector<Cell>& values);
  int64_t update_rows(const std::vector<int>& columns, const std::vector<Cell>& values,
                      const std::vector<Predicate>& predicates);
  int64_t delete_rows(const std::vector<Predicate>& predicates);

  // Primary key lookup. The page holding the row stays pinned until
  // release_row() is called.
  bool find_by_pk(int64_t pk, RowView* row);
  void release_row(const RowView& row);
  void register_pk(int64_t pk, RowLocation location);
  void unregister_pk(int64_t pk);
  const RowLocation* pk_location(int64_t pk) const;

  // Filters a batch of vector index candidates and measures their distance to
  // the query vector. The candidates are visited page by page, so that every
  // page of the buffer pool is pinned once instead of once per candidate.
  // The accepted primary keys and their distances are appended to the output.
  void filter_candidates(const std::vector<std::pair<int64_t, float>>& hits,
                         const std::vector<Predicate>& predicates, const float* query,
                         int dimension, std::vector<int64_t>* accepted,
                         std::vector<float>* distances);

  // Vector index.
  bool has_vector_index() const { return index_ != nullptr; }
  HnswIndex* vector_index() { return index_.get(); }
  void build_vector_index();
  void set_vector_index_enabled(bool enabled) { def_.indexed = enabled; }

  // Transaction boundaries for the in-memory indexes.
  void begin_statement();
  void commit_statement();
  void rollback_statement();
  void begin_transaction();
  void commit_transaction();
  void rollback_transaction();

  // Scans the table in physical order.
  class Scanner {
   public:
    explicit Scanner(Table& table);
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    bool next(RowView* row);

   private:
    void open_next_page();

    Table& table_;
    uint32_t next_page_ = 0;
    uint32_t current_page_ = 0;
    uint32_t slot_index_ = 0;
    uint16_t slot_count_ = 0;
    bool pinned_ = false;
    const uint8_t* page_data_ = nullptr;
  };

 private:
  void compute_layout();
  size_t row_size(const std::vector<Cell>& values) const;
  size_t encode_row(const std::vector<Cell>& values, uint8_t* target) const;
  bool place_row(uint32_t page_id, const uint8_t* payload, uint32_t length, uint32_t* slot_index);
  void write_page_header(uint32_t page_id, uint16_t slot_count, uint32_t heap_top,
                         uint32_t free_end);
  uint32_t table_id() const { return static_cast<uint32_t>(def_.id); }
  uint8_t* page_of(uint32_t page_id) { return storage_->page(table_id(), page_id); }
  void release_page(uint32_t page_id) { storage_->release(table_id(), page_id); }

  Storage* storage_ = nullptr;
  TableDef def_;
  uint64_t row_count_ = 0;
  uint32_t num_pages_ = 0;
  std::unordered_map<int64_t, RowLocation> pk_index_;
  std::unique_ptr<HnswIndex> index_;

  // Row layout, derived from the schema.
  uint32_t bigint_count_ = 0;
  uint32_t varchar_count_ = 0;
  uint32_t fixed_part_ = 0;
  uint32_t directory_offset_ = 0;
  uint32_t vector_offset_ = 0;
  uint32_t string_offset_ = 0;
  std::vector<uint32_t> int_offsets_;    // per column, byte offset of the BIGINT
  std::vector<uint32_t> str_offsets_;    // per column, byte offset of the length entry
  std::vector<uint32_t> str_ranks_;      // per column, rank among the VARCHAR columns
  std::vector<uint32_t> length_scratch_;
  std::vector<uint8_t> scratch_;
  std::vector<IndexUndo> index_undo_;
  std::vector<IndexUndo> txn_index_undo_;
  bool txn_active_ = false;
};

}  // namespace mvdb
