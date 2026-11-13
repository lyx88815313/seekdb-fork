#include "table.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mvdb {

namespace {

constexpr uint32_t kMaxIndexedRows = 60000;

inline void put_u16(uint8_t* p, uint16_t value) { std::memcpy(p, &value, 2); }
inline void put_u32(uint8_t* p, uint32_t value) { std::memcpy(p, &value, 4); }
inline void put_u64(uint8_t* p, uint64_t value) { std::memcpy(p, &value, 8); }
inline uint16_t get_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t get_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t get_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t align16(uint32_t value) { return (value + 15u) & ~15u; }

}  // namespace

Table::Table(Storage* storage, TableDef def) : storage_(storage), def_(std::move(def)) {
  compute_layout();
  if (def_.vector_index >= 0) {
    index_ = std::make_unique<HnswIndex>(static_cast<int>(def_.columns[def_.vector_index].length),
                                         1024);
  }
}

// ---------------------------------------------------------------------------
// Row layout
//
//   [0..2)    u16 null mask
//   [2..4)    u16 flags
//   [4..8)    u32 reserved
//   [8..16)   i64 primary key
//   [16..)    i64 for every other BIGINT column
//   u16 count of VARCHAR columns, then one u16 length per VARCHAR column
//   padding to a 16 byte boundary
//   float32 vector payload (the single vector column)
//   VARCHAR payloads
//
// The length directory keeps every VARCHAR column addressable in constant
// time even though the payloads are variable length.
// ---------------------------------------------------------------------------

void Table::compute_layout() {
  int_offsets_.assign(def_.columns.size(), 0);
  str_offsets_.assign(def_.columns.size(), 0);
  str_ranks_.assign(def_.columns.size(), 0xFFFFFFFFu);

  uint32_t bigints = 0;
  uint32_t varchars = 0;
  for (size_t i = 0; i < def_.columns.size(); ++i) {
    const ColumnDef& column = def_.columns[i];
    if (column.kind == ColKind::BigInt) {
      if (static_cast<int>(i) == def_.pk_index) {
        int_offsets_[i] = 8;
      } else {
        int_offsets_[i] = 16 + 8 * bigints;
        ++bigints;
      }
    } else if (column.kind == ColKind::VarChar) {
      str_ranks_[i] = varchars++;
    }
  }
  bigint_count_ = bigints;
  varchar_count_ = varchars;

  fixed_part_ = 16 + 8 * bigint_count_;
  directory_offset_ = fixed_part_;
  uint32_t body = align16(fixed_part_ + 2 + 2 * varchar_count_);
  if (def_.vector_index >= 0) {
    vector_offset_ = body;
    string_offset_ = body + 4 * def_.columns[def_.vector_index].length;
  } else {
    vector_offset_ = 0;
    string_offset_ = body;
  }
  for (size_t i = 0; i < def_.columns.size(); ++i) {
    if (str_ranks_[i] != 0xFFFFFFFFu) {
      str_offsets_[i] = directory_offset_ + 2 + 2 * str_ranks_[i];
    }
  }
}

size_t Table::row_size(const std::vector<Cell>& values) const {
  // The size has to match the layout computed by compute_layout() exactly:
  // the primary key already lives in the header, the directory is padded to
  // a 16 byte boundary, the vector payload is fixed size and the VARCHAR
  // payloads follow it.
  size_t strings = 0;
  for (size_t i = 0; i < def_.columns.size(); ++i) {
    if (def_.columns[i].kind == ColKind::VarChar) strings += values[i].text.size();
  }
  if (def_.vector_index >= 0) {
    return vector_offset_ + 4 * def_.columns[def_.vector_index].length + strings;
  }
  return string_offset_ + strings;
}

size_t Table::encode_row(const std::vector<Cell>& values, uint8_t* target) const {
  size_t total = row_size(values);
  std::memset(target, 0, total);

  uint16_t null_mask = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i].kind == Cell::Kind::Null) null_mask |= static_cast<uint16_t>(1u << i);
  }
  put_u16(target, null_mask);
  put_u16(target + 2, 0);
  int64_t pk = def_.pk_index >= 0 ? values[def_.pk_index].integer : 0;
  put_u64(target + 8, static_cast<uint64_t>(pk));

  uint32_t bigint_cursor = 16;
  uint32_t string_cursor = string_offset_;
  uint32_t varchar_cursor = 0;
  for (size_t i = 0; i < def_.columns.size(); ++i) {
    const ColumnDef& column = def_.columns[i];
    const Cell& value = values[i];
    switch (column.kind) {
      case ColKind::BigInt:
        if (static_cast<int>(i) != def_.pk_index) {
          put_u64(target + bigint_cursor, static_cast<uint64_t>(value.integer));
          bigint_cursor += 8;
        }
        break;
      case ColKind::VarChar: {
        put_u16(target + directory_offset_ + 2 + 2 * varchar_cursor,
                static_cast<uint16_t>(value.text.size()));
        if (!value.text.empty()) {
          std::memcpy(target + string_cursor, value.text.data(), value.text.size());
        }
        string_cursor += static_cast<uint32_t>(value.text.size());
        ++varchar_cursor;
        break;
      }
      case ColKind::Vector: {
        if (!value.vector.empty()) {
          std::memcpy(target + vector_offset_, value.vector.data(), 4 * value.vector.size());
        }
        break;
      }
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Row access
// ---------------------------------------------------------------------------

int64_t Table::row_int(const RowView& row, int column) const {
  int64_t value = 0;
  std::memcpy(&value, row.data + int_offsets_[column], 8);
  return value;
}

std::string Table::row_text(const RowView& row, int column) const {
  uint32_t length = 0;
  const char* text = row_text_view(row, column, &length);
  return std::string(text, length);
}

const char* Table::row_text_view(const RowView& row, int column, uint32_t* length) const {
  uint16_t stored = 0;
  std::memcpy(&stored, row.data + str_offsets_[column], 2);
  uint32_t offset = string_offset_;
  uint32_t rank = str_ranks_[column];
  for (uint32_t i = 0; i < rank; ++i) {
    uint16_t previous = 0;
    std::memcpy(&previous, row.data + directory_offset_ + 2 + 2 * i, 2);
    offset += previous;
  }
  if (length != nullptr) *length = stored;
  return reinterpret_cast<const char*>(row.data + offset);
}

const float* Table::row_vector(const RowView& row) const {
  if (def_.vector_index < 0) return nullptr;
  return reinterpret_cast<const float*>(row.data + vector_offset_);
}

bool Table::matches(const RowView& row, const std::vector<Predicate>& predicates) const {
  for (const Predicate& predicate : predicates) {
    const ColumnDef& column = def_.columns[predicate.column];
    int comparison = 0;
    if (predicate.is_text) {
      if (column.kind != ColKind::VarChar) return false;
      uint32_t length = 0;
      const char* text = row_text_view(row, predicate.column, &length);
      comparison = utf8_compare(text, length, predicate.text.data(), predicate.text.size());
    } else {
      if (column.kind != ColKind::BigInt) return false;
      int64_t value = row_int(row, predicate.column);
      if (value < predicate.integer) comparison = -1;
      else if (value > predicate.integer) comparison = 1;
      else comparison = 0;
    }
    switch (predicate.op) {
      case Compare::Op::Eq: if (comparison != 0) return false; break;
      case Compare::Op::Lt: if (comparison >= 0) return false; break;
      case Compare::Op::Le: if (comparison > 0) return false; break;
      case Compare::Op::Gt: if (comparison <= 0) return false; break;
      case Compare::Op::Ge: if (comparison < 0) return false; break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Primary key index
// ---------------------------------------------------------------------------

void Table::register_pk(int64_t pk, RowLocation location) { pk_index_[pk] = location; }

void Table::unregister_pk(int64_t pk) { pk_index_.erase(pk); }

const RowLocation* Table::pk_location(int64_t pk) const {
  auto it = pk_index_.find(pk);
  return it == pk_index_.end() ? nullptr : &it->second;
}

bool Table::find_by_pk(int64_t pk, RowView* row) {
  const RowLocation* location = pk_location(pk);
  if (location == nullptr) return false;
  uint8_t* page = page_of(location->page_id);
  uint64_t slot = get_u64(page + kPageHeaderSize + 8 * location->slot_index);
  uint32_t offset = slot_offset(slot);
  uint32_t length = slot_length(slot);
  if (offset == 0xFFFFFFFFu) {
    release_page(location->page_id);
    return false;
  }
  uint16_t null_mask = 0;
  std::memcpy(&null_mask, page + offset, 2);
  int64_t stored_pk = 0;
  std::memcpy(&stored_pk, page + offset + 8, 8);
  row->data = page + offset;
  row->length = length;
  row->null_mask = null_mask;
  row->pk = stored_pk;
  row->page_id = location->page_id;
  row->slot_index = location->slot_index;
  return true;  // the page stays pinned until release_row() is called
}

void Table::release_row(const RowView& row) { release_page(row.page_id); }

void Table::filter_candidates(const std::vector<std::pair<int64_t, float>>& hits,
                              const std::vector<Predicate>& predicates, const float* query,
                              int dimension, std::vector<int64_t>* accepted,
                              std::vector<float>* distances) {
  // Group the candidates by the page that holds them, so that every page is
  // pinned only once.
  struct Located {
    size_t hit = 0;
    uint32_t page_id = 0;
  };
  std::vector<Located> located;
  located.reserve(hits.size());
  for (size_t i = 0; i < hits.size(); ++i) {
    const RowLocation* location = pk_location(hits[i].first);
    if (location == nullptr) continue;
    located.push_back(Located{i, location->page_id});
  }
  std::sort(located.begin(), located.end(),
            [](const Located& left, const Located& right) { return left.page_id < right.page_id; });

  size_t index = 0;
  while (index < located.size()) {
    const uint32_t page_id = located[index].page_id;
    size_t end = index;
    while (end < located.size() && located[end].page_id == page_id) ++end;
    uint8_t* page = page_of(page_id);
    for (size_t i = index; i < end; ++i) {
      const Located& item = located[i];
      const RowLocation* location = pk_location(hits[item.hit].first);
      if (location == nullptr) continue;
      uint64_t slot = get_u64(page + kPageHeaderSize + 8 * location->slot_index);
      uint32_t offset = slot_offset(slot);
      if (offset == 0xFFFFFFFFu) continue;  // the row was deleted
      RowView row;
      row.data = page + offset;
      row.length = slot_length(slot);
      row.pk = hits[item.hit].first;
      row.page_id = page_id;
      row.slot_index = location->slot_index;
      if (!matches(row, predicates)) continue;
      const float* vector = row_vector(row);
      float distance = vector != nullptr ? l2_distance(vector, query, dimension) : 0.0f;
      accepted->push_back(row.pk);
      distances->push_back(distance);
    }
    release_page(page_id);
    index = end;
  }
}

// ---------------------------------------------------------------------------
// Page helpers
// ---------------------------------------------------------------------------

void Table::write_page_header(uint32_t page_id, uint16_t slot_count, uint32_t heap_top,
                              uint32_t free_end) {
  uint8_t block[12];
  uint8_t* page = page_of(page_id);
  uint16_t page_type = get_u16(page + kHdrPageType);
  std::memcpy(block, &page_type, 2);
  std::memcpy(block + 2, &slot_count, 2);
  std::memcpy(block + 4, &heap_top, 4);
  std::memcpy(block + 8, &free_end, 4);
  storage_->write_bytes(table_id(), page_id, kHdrPageType, 12, block);
  release_page(page_id);
}

bool Table::place_row(uint32_t page_id, const uint8_t* payload, uint32_t length,
                      uint32_t* slot_index) {
  uint8_t* page = page_of(page_id);
  uint32_t heap_top = get_u32(page + kHdrHeapTop);
  uint32_t free_end = get_u32(page + kHdrFreeEnd);
  uint16_t slot_count = get_u16(page + kHdrSlotCount);
  uint32_t aligned = align16(length);
  if (heap_top < free_end || heap_top - free_end < aligned + 8) {
    release_page(page_id);
    return false;
  }
  uint32_t offset = heap_top - aligned;
  uint64_t slot = make_slot(offset, length);
  storage_->write_bytes(table_id(), page_id, offset, length, payload);
  storage_->write_bytes(table_id(), page_id, kPageHeaderSize + 8 * slot_count, 8, &slot);
  write_page_header(page_id, static_cast<uint16_t>(slot_count + 1), offset, free_end + 8);
  *slot_index = slot_count;
  release_page(page_id);
  return true;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

int64_t Table::insert_row(const std::vector<Cell>& values) {
  if (values.size() != def_.columns.size()) {
    throw_error("column_count_mismatch",
                "expected " + std::to_string(def_.columns.size()) + " values but got " +
                    std::to_string(values.size()));
  }
  for (size_t i = 0; i < values.size(); ++i) {
    const ColumnDef& column = def_.columns[i];
    const Cell& value = values[i];
    if (value.kind == Cell::Kind::Null) {
      throw_error("unsupported_sql", "NULL values are not supported");
    }
    switch (column.kind) {
      case ColKind::BigInt:
        if (value.kind != Cell::Kind::Int) {
          throw_error("type_mismatch", "column " + column.name + " expects a BIGINT");
        }
        break;
      case ColKind::VarChar:
        if (value.kind != Cell::Kind::Str) {
          throw_error("type_mismatch", "column " + column.name + " expects a string");
        }
        if (utf8_length(value.text) > column.length) {
          throw_error("value_too_long", "value for column " + column.name +
                                           " exceeds VARCHAR(" + std::to_string(column.length) +
                                           ")");
        }
        break;
      case ColKind::Vector:
        if (value.kind != Cell::Kind::Vector) {
          throw_error("invalid_vector", "column " + column.name + " expects a vector");
        }
        if (value.vector.size() != column.length) {
          throw_error("dimension_mismatch", "column " + column.name + " expects VECTOR(" +
                                                std::to_string(column.length) + ") but got " +
                                                std::to_string(value.vector.size()) +
                                                " components");
        }
        for (float component : value.vector) {
          if (!std::isfinite(component)) {
            throw_error("invalid_vector", "vector components must be finite");
          }
        }
        break;
    }
  }

  int64_t pk = values[def_.pk_index].integer;
  if (pk_index_.find(pk) != pk_index_.end()) {
    throw_error("duplicate_key", "duplicate primary key " + std::to_string(pk));
  }

  size_t length = row_size(values);
  scratch_.resize(length);
  encode_row(values, scratch_.data());

  uint32_t page_id = num_pages_ > 0 ? num_pages_ - 1 : 0xFFFFFFFFu;
  uint32_t slot_index = 0;
  bool placed = false;
  if (page_id != 0xFFFFFFFFu) {
    placed = place_row(page_id, scratch_.data(), static_cast<uint32_t>(length), &slot_index);
  }
  if (!placed) {
    page_id = storage_->alloc_page(table_id());
    num_pages_ = page_id + 1;
    if (!place_row(page_id, scratch_.data(), static_cast<uint32_t>(length), &slot_index)) {
      throw_error("internal_error", "cannot place a row into a fresh page");
    }
  }
  register_pk(pk, RowLocation{page_id, slot_index});
  ++row_count_;
  IndexUndo undo;
  undo.pk = pk;
  undo.had_location = false;
  undo.had_vector = false;
  index_undo_.push_back(std::move(undo));
  if (index_ != nullptr && row_count_ <= kMaxIndexedRows) {
    index_->set(pk, reinterpret_cast<const float*>(scratch_.data() + vector_offset_));
  }
  return 1;
}

int64_t Table::update_rows(const std::vector<int>& columns, const std::vector<Cell>& values,
                           const std::vector<Predicate>& predicates) {
  for (int column : columns) {
    if (column == def_.pk_index) {
      throw_error("unsupported_sql", "the primary key cannot be updated");
    }
  }

  // Collect the affected primary keys first so that a failing statement never
  // leaves half of its changes behind.
  std::vector<int64_t> victims;
  {
    Scanner scanner(*this);
    RowView row;
    while (scanner.next(&row)) {
      if (matches(row, predicates)) victims.push_back(row.pk);
    }
  }

  int64_t affected = 0;
  std::vector<uint8_t> encoded;
  for (int64_t pk : victims) {
    RowView live;
    if (!find_by_pk(pk, &live)) continue;
    const RowLocation* before = pk_location(pk);
    IndexUndo undo;
    undo.pk = pk;
    undo.had_location = before != nullptr;
    if (before != nullptr) undo.location = *before;
    std::vector<Cell> current(def_.columns.size());
    for (size_t i = 0; i < def_.columns.size(); ++i) {
      const ColumnDef& column = def_.columns[i];
      switch (column.kind) {
        case ColKind::BigInt:
          current[i] = Cell::make_int(row_int(live, static_cast<int>(i)));
          break;
        case ColKind::VarChar:
          current[i] = Cell::make_text(row_text(live, static_cast<int>(i)));
          break;
        case ColKind::Vector: {
          const float* source = row_vector(live);
          std::vector<float> copy(source, source + column.length);
          current[i] = Cell::make_vector(std::move(copy));
          break;
        }
      }
    }
    if (def_.vector_index >= 0) {
      undo.had_vector = true;
      undo.vector = current[def_.vector_index].vector;
    }
    for (size_t i = 0; i < columns.size(); ++i) {
      const ColumnDef& column = def_.columns[columns[i]];
      const Cell& value = values[i];
      switch (column.kind) {
        case ColKind::BigInt:
          if (value.kind != Cell::Kind::Int) {
            throw_error("type_mismatch", "column " + column.name + " expects a BIGINT");
          }
          current[columns[i]] = Cell::make_int(value.integer);
          break;
        case ColKind::VarChar:
          if (value.kind != Cell::Kind::Str) {
            throw_error("type_mismatch", "column " + column.name + " expects a string");
          }
          if (utf8_length(value.text) > column.length) {
            throw_error("value_too_long",
                        "value for column " + column.name + " exceeds VARCHAR(" +
                            std::to_string(column.length) + ")");
          }
          current[columns[i]] = Cell::make_text(value.text);
          break;
        case ColKind::Vector:
          if (value.kind != Cell::Kind::Vector) {
            throw_error("invalid_vector", "column " + column.name + " expects a vector");
          }
          if (value.vector.size() != column.length) {
            throw_error("dimension_mismatch",
                        "column " + column.name + " expects VECTOR(" +
                            std::to_string(column.length) + ") but got " +
                            std::to_string(value.vector.size()) + " components");
          }
          current[columns[i]] = Cell::make_vector(value.vector);
          break;
      }
    }

    size_t length = row_size(current);
    encoded.resize(length);
    encode_row(current, encoded.data());

    uint8_t* page = page_of(live.page_id);
    uint64_t slot = get_u64(page + kPageHeaderSize + 8 * live.slot_index);
    uint32_t old_offset = slot_offset(slot);
    uint32_t old_length = slot_length(slot);
    uint32_t heap_top = get_u32(page + kHdrHeapTop);
    uint16_t slot_count = get_u16(page + kHdrSlotCount);
    uint32_t free_end = get_u32(page + kHdrFreeEnd);

    if (length <= old_length) {
      // The row keeps its slot and the new payload overwrites the old one.
      storage_->write_bytes(table_id(), live.page_id, old_offset,
                            static_cast<uint32_t>(length), encoded.data());
      uint64_t new_slot = make_slot(old_offset, static_cast<uint32_t>(length));
      storage_->write_bytes(table_id(), live.page_id, kPageHeaderSize + 8 * live.slot_index, 8,
                            &new_slot);
      if (old_offset + old_length == heap_top) {
        write_page_header(live.page_id, slot_count, old_offset + old_length, free_end);
      }
    } else {
      // The row outgrew its slot, so it moves into the free space and the old
      // slot is released.
      uint32_t slot_index = 0;
      uint32_t target_page = live.page_id;
      bool placed = place_row(target_page, encoded.data(), static_cast<uint32_t>(length),
                              &slot_index);
      if (!placed) {
        target_page = storage_->alloc_page(table_id());
        num_pages_ = target_page + 1;
        placed = place_row(target_page, encoded.data(), static_cast<uint32_t>(length),
                           &slot_index);
      }
      if (!placed) throw_error("internal_error", "cannot store the updated row");
      uint64_t dead = kSlotFree;
      storage_->write_bytes(table_id(), live.page_id, kPageHeaderSize + 8 * live.slot_index, 8,
                            &dead);
      register_pk(pk, RowLocation{target_page, slot_index});
    }
    if (index_ != nullptr) {
      index_->set(pk, reinterpret_cast<const float*>(encoded.data() + vector_offset_));
    }
    index_undo_.push_back(std::move(undo));
    release_row(live);
    ++affected;
  }
  return affected;
}

int64_t Table::delete_rows(const std::vector<Predicate>& predicates) {
  std::vector<int64_t> victims;
  std::vector<std::vector<float>> vectors;
  {
    Scanner scanner(*this);
    RowView row;
    while (scanner.next(&row)) {
      if (!matches(row, predicates)) {
        continue;
      }
      victims.push_back(row.pk);
      if (def_.vector_index >= 0 && index_ != nullptr) {
        const float* source = row_vector(row);
        vectors.push_back(std::vector<float>(source, source + def_.columns[def_.vector_index].length));
      } else {
        vectors.emplace_back();
      }
    }
  }
  int64_t affected = 0;
  for (size_t i = 0; i < victims.size(); ++i) {
    int64_t pk = victims[i];
    const RowLocation* location = pk_location(pk);
    if (location == nullptr) continue;
    uint32_t page_id = location->page_id;
    uint32_t slot_index = location->slot_index;
    page_of(page_id);  // pins the page while it is updated
    uint64_t dead = kSlotFree;
    storage_->write_bytes(table_id(), page_id, kPageHeaderSize + 8 * slot_index, 8, &dead);
    release_page(page_id);
    IndexUndo undo;
    undo.pk = pk;
    undo.had_location = true;
    undo.location = *location;
    undo.had_vector = def_.vector_index >= 0 && index_ != nullptr;
    undo.vector = vectors[i];
    index_undo_.push_back(std::move(undo));
    unregister_pk(pk);
    if (index_ != nullptr) index_->remove(pk);
    --row_count_;
    ++affected;
  }
  return affected;
}

// ---------------------------------------------------------------------------
// Transaction boundaries for the in-memory indexes
// ---------------------------------------------------------------------------

void Table::begin_statement() { index_undo_.clear(); }

void Table::commit_statement() {
  if (txn_active_) {
    for (IndexUndo& undo : index_undo_) txn_index_undo_.push_back(std::move(undo));
  }
  index_undo_.clear();
}

void Table::rollback_statement() {
  for (auto it = index_undo_.rbegin(); it != index_undo_.rend(); ++it) {
    const IndexUndo& undo = *it;
    if (undo.had_location) {
      register_pk(undo.pk, undo.location);
    } else {
      unregister_pk(undo.pk);
    }
    if (index_ != nullptr) {
      if (undo.had_vector) {
        index_->set(undo.pk, undo.vector.data());
      } else {
        index_->remove(undo.pk);
      }
    }
  }
  index_undo_.clear();
}

void Table::begin_transaction() {
  txn_active_ = true;
  txn_index_undo_.clear();
}

void Table::commit_transaction() {
  txn_active_ = false;
  txn_index_undo_.clear();
}

void Table::rollback_transaction() {
  for (auto it = txn_index_undo_.rbegin(); it != txn_index_undo_.rend(); ++it) {
    const IndexUndo& undo = *it;
    if (undo.had_location) {
      register_pk(undo.pk, undo.location);
    } else {
      unregister_pk(undo.pk);
    }
    if (index_ != nullptr) {
      if (undo.had_vector) {
        index_->set(undo.pk, undo.vector.data());
      } else {
        index_->remove(undo.pk);
      }
    }
  }
  txn_active_ = false;
  txn_index_undo_.clear();
}

// ---------------------------------------------------------------------------
// Vector index
// ---------------------------------------------------------------------------

void Table::build_vector_index() {
  if (def_.vector_index < 0 || index_ == nullptr) return;
  std::vector<int64_t> labels;
  // The vectors are copied out of the pages: the scan releases every page it
  // leaves, so a pointer into a page would be dangling by the time the index
  // is rebuilt.
  std::vector<std::vector<float>> owned;
  {
    Scanner scanner(*this);
    RowView row;
    while (scanner.next(&row)) {
      labels.push_back(row.pk);
      const float* source = row_vector(row);
      owned.emplace_back(source, source + def_.columns[def_.vector_index].length);
    }
  }
  std::vector<const float*> vectors;
  vectors.reserve(owned.size());
  for (const std::vector<float>& item : owned) vectors.push_back(item.data());
  index_->rebuild(labels, vectors);
}

// ---------------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------------

Table::Scanner::Scanner(Table& table) : table_(table) {}

Table::Scanner::~Scanner() {
  if (pinned_) table_.release_page(current_page_);
}

void Table::Scanner::open_next_page() {
  if (pinned_) {
    table_.release_page(current_page_);
    pinned_ = false;
  }
  if (next_page_ >= table_.num_pages_) return;
  uint8_t* page = table_.page_of(next_page_);
  current_page_ = next_page_;
  ++next_page_;
  pinned_ = true;
  page_data_ = page;
  slot_count_ = get_u16(page + kHdrSlotCount);
  slot_index_ = 0;
}

bool Table::Scanner::next(RowView* row) {
  while (true) {
    if (!pinned_) {
      if (next_page_ >= table_.num_pages_) return false;
      open_next_page();
      if (!pinned_) return false;
    }
    if (slot_index_ >= slot_count_) {
      open_next_page();
      continue;
    }
    uint64_t slot = get_u64(page_data_ + kPageHeaderSize + 8 * slot_index_);
    uint32_t offset = slot_offset(slot);
    uint32_t length = slot_length(slot);
    uint32_t current_slot = slot_index_;
    ++slot_index_;
    if (offset == 0xFFFFFFFFu) continue;  // released slot
    uint16_t null_mask = 0;
    std::memcpy(&null_mask, page_data_ + offset, 2);
    int64_t pk = 0;
    std::memcpy(&pk, page_data_ + offset + 8, 8);
    row->data = page_data_ + offset;
    row->length = length;
    row->null_mask = null_mask;
    row->pk = pk;
    row->page_id = current_page_;
    row->slot_index = current_slot;
    return true;
  }
}

}  // namespace mvdb
