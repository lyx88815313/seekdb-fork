#include "db.hpp"

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <sstream>

namespace mvdb {

namespace {

// A sortable key for ORDER BY.
struct SortKey {
  enum class Kind { Integer, Number, Text };
  Kind kind = Kind::Integer;
  int64_t integer = 0;
  double number = 0;
  uint32_t text_offset = 0;  // offset into the shared text arena
  uint32_t text_length = 0;
};

// One row produced by a scan.
struct Candidate {
  int64_t pk = 0;
  uint32_t page_id = 0;
  uint32_t slot_index = 0;
  uint32_t key_offset = 0;  // offset into the flat key array
  uint32_t key_count = 0;
};

bool key_less(const SortKey& left, const SortKey& right, const std::string& arena) {
  if (left.kind != right.kind) return left.kind < right.kind;
  switch (left.kind) {
    case SortKey::Kind::Integer:
      return left.integer < right.integer;
    case SortKey::Kind::Number:
      return left.number < right.number;
    case SortKey::Kind::Text:
      return utf8_compare(arena.data() + left.text_offset, left.text_length,
                          arena.data() + right.text_offset, right.text_length) < 0;
  }
  return false;
}

}  // namespace

Database::Database(std::string data_dir) : storage_(std::move(data_dir)) {}

Database::~Database() { close(); }

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

std::string Database::serialize_def(const TableDef& def, uint64_t row_count) const {
  std::ostringstream out;
  out << def.name << ';' << def.pk_index << ';' << def.vector_index << ';' << (def.indexed ? 1 : 0)
      << ';' << row_count;
  for (const ColumnDef& column : def.columns) {
    out << ';' << static_cast<int>(column.kind) << ',' << column.length << ','
        << (column.primary_key ? 1 : 0) << ',' << column.name;
  }
  return out.str();
}

bool Database::parse_def(const std::string& text, TableDef* def, uint64_t* row_count) const {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(';', start);
    if (end == std::string::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  if (parts.size() < 5) return false;
  def->name = parts[0];
  def->pk_index = std::atoi(parts[1].c_str());
  def->vector_index = std::atoi(parts[2].c_str());
  def->indexed = parts[3] == "1";
  if (row_count != nullptr) *row_count = std::strtoull(parts[4].c_str(), nullptr, 10);
  def->columns.clear();
  for (size_t i = 5; i < parts.size(); ++i) {
    const std::string& entry = parts[i];
    size_t first = entry.find(',');
    if (first == std::string::npos) return false;
    size_t second = entry.find(',', first + 1);
    if (second == std::string::npos) return false;
    size_t third = entry.find(',', second + 1);
    if (third == std::string::npos) return false;
    ColumnDef column;
    column.kind = static_cast<ColKind>(std::atoi(entry.substr(0, first).c_str()));
    column.length = static_cast<uint32_t>(std::atoi(entry.substr(first + 1, second - first - 1).c_str()));
    column.primary_key = entry.substr(second + 1, third - second - 1) == "1";
    column.name = entry.substr(third + 1);
    def->columns.push_back(std::move(column));
  }
  return true;
}

void Database::bootstrap_catalog() {
  TableDef def;
  def.id = kCatalogId;
  def.name = "__catalog__";
  ColumnDef id_column;
  id_column.name = "id";
  id_column.kind = ColKind::BigInt;
  id_column.primary_key = true;
  ColumnDef def_column;
  def_column.name = "def";
  def_column.kind = ColKind::VarChar;
  def_column.length = 8192;
  def.columns.push_back(id_column);
  def.columns.push_back(def_column);
  def.pk_index = 0;
  def.vector_index = -1;
  catalog_ = std::make_unique<Table>(&storage_, def);
  // The marker row guarantees that the catalog file exists on disk.
  std::vector<Cell> values{Cell::make_int(0), Cell::make_text("__catalog__;0;-1;0;0")};
  catalog_->insert_row(values);
}

void Database::load_catalog() {
  if (!storage_.table_exists(static_cast<uint32_t>(kCatalogId))) {
    bootstrap_catalog();
    return;
  }
  TableDef def;
  def.id = kCatalogId;
  def.name = "__catalog__";
  ColumnDef id_column;
  id_column.name = "id";
  id_column.kind = ColKind::BigInt;
  id_column.primary_key = true;
  ColumnDef def_column;
  def_column.name = "def";
  def_column.kind = ColKind::VarChar;
  def_column.length = 8192;
  def.columns.push_back(id_column);
  def.columns.push_back(def_column);
  def.pk_index = 0;
  def.vector_index = -1;
  catalog_ = std::make_unique<Table>(&storage_, def);
  catalog_->set_page_count(static_cast<uint32_t>(storage_.page_count(kCatalogId)));

  // The catalog is scanned directly: the primary key index of the catalog is
  // not built yet at this point, so the rows cannot be looked up by key.
  std::vector<int64_t> ids;
  std::vector<std::string> definitions;
  {
    Table::Scanner scanner(*catalog_);
    RowView row;
    while (scanner.next(&row)) {
      ids.push_back(row.pk);
      definitions.push_back(row.pk == kCatalogId ? std::string() : catalog_->row_text(row, 1));
      catalog_->register_pk(row.pk, RowLocation{row.page_id, row.slot_index});
    }
  }
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == kCatalogId) continue;
    TableDef table_def;
    uint64_t row_count = 0;
    if (!parse_def(definitions[i], &table_def, &row_count)) continue;
    table_def.id = ids[i];
    auto table = std::make_unique<Table>(&storage_, table_def);
    table->set_page_count(static_cast<uint32_t>(storage_.page_count(static_cast<uint32_t>(ids[i]))));
    table->set_row_count(row_count);
    tables_.push_back(std::move(table));
  }
}

void Database::write_catalog_entry(const TableDef& def, uint64_t row_count) {
  const std::string serialized = serialize_def(def, row_count);
  const RowLocation* existing = catalog_->pk_location(def.id);
  if (existing != nullptr) {
    // update_rows() expects the values to be parallel to the changed column
    // list, so only the definition column is handed over.
    std::vector<int> columns{1};
    std::vector<Cell> values{Cell::make_text(serialized)};
    std::vector<Predicate> predicates;
    Predicate predicate;
    predicate.column = 0;
    predicate.op = Compare::Op::Eq;
    predicate.integer = def.id;
    predicates.push_back(predicate);
    catalog_->update_rows(columns, values, predicates);
  } else {
    std::vector<Cell> values{Cell::make_int(def.id), Cell::make_text(serialized)};
    catalog_->insert_row(values);
  }
}

void Database::refresh_catalog_entry(Table* table) {
  TableDef def = table->def();
  write_catalog_entry(def, table->row_count());
}

Table* Database::find_table(const std::string& name) {
  for (auto& table : tables_) {
    if (table->def().name == name) return table.get();
  }
  return nullptr;
}

Table* Database::table_by_id(int64_t id) {
  for (auto& table : tables_) {
    if (table->def().id == id) return table.get();
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Database::open() {
  storage_.open();
  // The start up writes (the catalog bootstrap and the refreshed row counts)
  // are collected in one statement so that they end up in the write ahead log.
  storage_.begin_statement();
  load_catalog();
  for (auto& table : tables_) table->begin_statement();
  catalog_->begin_statement();
  // Rebuild the in-memory indexes. The primary key index and the vector
  // index are derived structures and are rebuilt from the persisted rows.
  for (auto& table : tables_) {
    uint64_t rows = 0;
    bool has_vector = table->def().vector_index >= 0;
    {
      Table::Scanner scanner(*table);
      RowView row;
      while (scanner.next(&row)) {
        table->register_pk(row.pk, RowLocation{row.page_id, row.slot_index});
        ++rows;
      }
    }
    table->set_row_count(rows);
    if (has_vector && table->row_count() <= 60000) {
      // The index is a derived structure: it is rebuilt from the persisted
      // rows. The database is only ready once the rebuild is finished.
      const auto started = std::chrono::steady_clock::now();
      table->build_vector_index();
      const auto finished = std::chrono::steady_clock::now();
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
      std::fprintf(stderr, "minivectordb: rebuilt the vector index of table %s (%llu rows) in %lld ms\n",
                   table->def().name.c_str(),
                   static_cast<unsigned long long>(table->row_count()),
                   static_cast<long long>(micros / 1000));
    }
    refresh_catalog_entry(table.get());
  }
  for (auto& table : tables_) table->commit_statement();
  catalog_->commit_statement();
  storage_.commit_statement();
  opened_ = true;
}

void Database::close() {
  if (!opened_) return;
  opened_ = false;
  if (explicit_txn_) {
    // A normal shutdown rolls back the transaction that never committed.
    storage_.rollback_transaction();
    for (auto& table : tables_) table->rollback_transaction();
    catalog_->rollback_transaction();
    explicit_txn_ = false;
  }
  storage_.close();
}

// ---------------------------------------------------------------------------
// Statement execution
// ---------------------------------------------------------------------------

Database::Result Database::execute(const std::string& sql, bool ann_mode) {
  Statement statement = parse_sql(sql);

  if (statement.kind == StmtKind::Begin) {
    if (explicit_txn_) {
      throw_error("unsupported_sql", "nested transactions are not supported");
    }
    explicit_txn_ = true;
    storage_.begin_transaction();
    for (auto& table : tables_) table->begin_transaction();
    catalog_->begin_transaction();
    Result result;
    return result;
  }
  if (statement.kind == StmtKind::Commit) {
    if (!explicit_txn_) {
      Result result;
      return result;  // committing without a transaction is a no-op
    }
    storage_.commit_transaction();
    for (auto& table : tables_) table->commit_transaction();
    catalog_->commit_transaction();
    explicit_txn_ = false;
    Result result;
    return result;
  }
  if (statement.kind == StmtKind::Rollback) {
    if (!explicit_txn_) {
      Result result;
      return result;  // rolling back without a transaction is a no-op
    }
    storage_.rollback_transaction();
    for (auto& table : tables_) table->rollback_transaction();
    catalog_->rollback_transaction();
    explicit_txn_ = false;
    Result result;
    return result;
  }

  storage_.begin_statement();
  for (auto& table : tables_) table->begin_statement();
  catalog_->begin_statement();
  try {
    Result result = execute_statement(statement, ann_mode);
    storage_.commit_statement();
    for (auto& table : tables_) table->commit_statement();
    catalog_->commit_statement();
    return result;
  } catch (...) {
    storage_.rollback_statement();
    for (auto& table : tables_) table->rollback_statement();
    catalog_->rollback_statement();
    throw;
  }
}

Database::Result Database::execute_statement(const Statement& statement, bool ann_mode) {
  switch (statement.kind) {
    case StmtKind::CreateTable: return execute_create_table(statement);
    case StmtKind::CreateVectorIndex: return execute_create_index(statement);
    case StmtKind::DropTable: return execute_drop_table(statement);
    case StmtKind::Insert: return execute_insert(statement);
    case StmtKind::Select: return execute_select(statement, ann_mode);
    case StmtKind::Update: return execute_update(statement);
    case StmtKind::Delete: return execute_delete(statement);
    default: throw_error("unsupported_sql", "unsupported statement");
  }
  Result result;
  return result;
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------

Database::Result Database::execute_create_table(const Statement& statement) {
  if (find_table(statement.table_name) != nullptr) {
    throw_error("table_exists", "table " + statement.table_name + " already exists");
  }
  int pk_index = -1;
  int vector_index = -1;
  std::vector<std::string> names;
  for (size_t i = 0; i < statement.columns.size(); ++i) {
    const ColumnDef& column = statement.columns[i];
    if (column.name.empty()) throw_error("syntax_error", "empty column name");
    for (const std::string& name : names) {
      if (name == column.name) {
        throw_error("duplicate_column", "column " + column.name + " is declared twice");
      }
    }
    names.push_back(column.name);
    if (column.primary_key) {
      if (pk_index >= 0) {
        throw_error("syntax_error", "only one primary key is allowed");
      }
      if (column.kind != ColKind::BigInt) {
        throw_error("syntax_error", "the primary key must be a BIGINT");
      }
      pk_index = static_cast<int>(i);
    }
    if (column.kind == ColKind::Vector) {
      if (vector_index >= 0) {
        throw_error("syntax_error", "only one vector column is allowed");
      }
      vector_index = static_cast<int>(i);
    }
    if (column.kind == ColKind::VarChar && column.length == 0) {
      throw_error("syntax_error", "VARCHAR length must be positive");
    }
  }
  if (pk_index < 0) {
    throw_error("syntax_error", "a table needs exactly one BIGINT primary key");
  }
  int64_t next_id = 1;
  for (auto& table : tables_) {
    if (table->def().id >= next_id) next_id = table->def().id + 1;
  }

  TableDef def;
  def.id = next_id;
  def.name = statement.table_name;
  def.columns = statement.columns;
  def.pk_index = pk_index;
  def.vector_index = vector_index;
  def.indexed = vector_index >= 0;

  auto table = std::make_unique<Table>(&storage_, def);
  tables_.push_back(std::move(table));
  write_catalog_entry(def, 0);
  Result result;
  return result;
}

Database::Result Database::execute_create_index(const Statement& statement) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  int column = table->def().index_of(statement.index_column_name);
  if (column < 0) {
    throw_error("column_not_found", "unknown column " + statement.index_column_name);
  }
  if (table->def().vector_index < 0 || column != table->def().vector_index) {
    throw_error("syntax_error", "a vector index can only be built on the vector column");
  }
  table->set_vector_index_enabled(true);
  table->build_vector_index();
  refresh_catalog_entry(table);
  Result result;
  return result;
}

Database::Result Database::execute_drop_table(const Statement& statement) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  int64_t id = table->def().id;
  std::vector<int> columns{1};
  std::vector<Predicate> predicates;
  Predicate predicate;
  predicate.column = 0;
  predicate.op = Compare::Op::Eq;
  predicate.integer = id;
  predicates.push_back(predicate);
  catalog_->update_rows(columns, {Cell::make_text("")}, predicates);
  for (auto it = tables_.begin(); it != tables_.end(); ++it) {
    if ((*it)->def().id == id) {
      tables_.erase(it);
      break;
    }
  }
  Result result;
  return result;
}

// ---------------------------------------------------------------------------
// DML
// ---------------------------------------------------------------------------

Database::Result Database::execute_insert(const Statement& statement) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  const TableDef& def = table->def();

  std::vector<int> targets;
  if (statement.insert_column_names.empty()) {
    for (size_t i = 0; i < def.columns.size(); ++i) targets.push_back(static_cast<int>(i));
  } else {
    for (const std::string& name : statement.insert_column_names) {
      int index = def.index_of(name);
      if (index < 0) {
        throw_error("column_not_found", "unknown column " + name);
      }
      for (int existing : targets) {
        if (existing == index) {
          throw_error("duplicate_column", "column " + name + " is listed twice");
        }
      }
      targets.push_back(index);
    }
  }

  int64_t affected = 0;
  for (const std::vector<Literal>& row : statement.insert_rows) {
    if (row.size() != targets.size()) {
      throw_error("column_count_mismatch",
                  "expected " + std::to_string(targets.size()) + " values but got " +
                      std::to_string(row.size()));
    }
    std::vector<Cell> values(def.columns.size());
    // Columns that the statement does not mention receive the zero value of
    // their type. NULL is not part of the supported SQL subset, so there is
    // no "unknown" value to fall back to.
    for (size_t i = 0; i < def.columns.size(); ++i) {
      switch (def.columns[i].kind) {
        case ColKind::BigInt: values[i] = Cell::make_int(0); break;
        case ColKind::VarChar: values[i] = Cell::make_text(""); break;
        case ColKind::Vector:
          values[i] = Cell::make_vector(std::vector<float>(def.columns[i].length, 0.0f));
          break;
      }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
      int column = targets[i];
      const Literal& literal = row[i];
      const ColumnDef& definition = def.columns[column];
      switch (literal.kind) {
        case Literal::Kind::Int:
          if (definition.kind != ColKind::BigInt) {
            throw_error("type_mismatch", "column " + definition.name + " expects a " +
                                             (definition.kind == ColKind::VarChar ? "string" : "vector"));
          }
          values[column] = Cell::make_int(literal.integer);
          break;
        case Literal::Kind::Double:
          if (definition.kind != ColKind::BigInt) {
            throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
          }
          if (literal.number != std::floor(literal.number) || !std::isfinite(literal.number)) {
            throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
          }
          values[column] = Cell::make_int(static_cast<int64_t>(literal.number));
          break;
        case Literal::Kind::Str:
          if (definition.kind != ColKind::VarChar) {
            throw_error("type_mismatch", "column " + definition.name + " expects a string");
          }
          values[column] = Cell::make_text(literal.text);
          break;
        case Literal::Kind::Vector:
          if (definition.kind != ColKind::Vector) {
            throw_error("invalid_vector", "column " + definition.name + " expects a string");
          }
          values[column] = Cell::make_vector(literal.vector);
          break;
        case Literal::Kind::Null:
          throw_error("unsupported_sql", "NULL values are not supported");
      }
    }
    affected += table->insert_row(values);
  }
  refresh_catalog_entry(table);
  Result result;
  result.affected_rows = affected;
  return result;
}

std::vector<Predicate> Database::resolve_predicates(const Statement& statement,
                                                    const TableDef& def) {
  std::vector<Predicate> predicates;
  for (const Compare& compare : statement.where) {
    int column = def.index_of(compare.column_name);
    if (column < 0) {
      throw_error("column_not_found", "unknown column " + compare.column_name);
    }
    const ColumnDef& definition = def.columns[column];
    Predicate predicate;
    predicate.column = column;
    predicate.op = compare.op;
    switch (compare.value.kind) {
      case Literal::Kind::Int:
        if (definition.kind != ColKind::BigInt) {
          throw_error("type_mismatch", "column " + definition.name + " expects a string");
        }
        predicate.integer = compare.value.integer;
        predicate.is_text = false;
        break;
      case Literal::Kind::Double:
        if (definition.kind != ColKind::BigInt) {
          throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
        }
        predicate.integer = static_cast<int64_t>(compare.value.number);
        predicate.is_text = false;
        break;
      case Literal::Kind::Str:
        if (definition.kind != ColKind::VarChar) {
          throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
        }
        predicate.text = compare.value.text;
        predicate.is_text = true;
        break;
      case Literal::Kind::Vector:
        throw_error("unsupported_sql", "vector columns cannot be used in a comparison");
      case Literal::Kind::Null:
        throw_error("unsupported_sql", "NULL values are not supported");
    }
    predicates.push_back(std::move(predicate));
  }
  return predicates;
}

Database::Result Database::execute_update(const Statement& statement) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  const TableDef& def = table->def();
  std::vector<int> columns;
  std::vector<Cell> values;
  for (const auto& assignment : statement.assignments) {
    int column = def.index_of(assignment.first);
    if (column < 0) {
      throw_error("column_not_found", "unknown column " + assignment.first);
    }
    const ColumnDef& definition = def.columns[column];
    const Literal& literal = assignment.second;
    Cell value;
    switch (literal.kind) {
      case Literal::Kind::Int:
        if (definition.kind != ColKind::BigInt) {
          throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
        }
        value = Cell::make_int(literal.integer);
        break;
      case Literal::Kind::Double:
        if (definition.kind != ColKind::BigInt) {
          throw_error("type_mismatch", "column " + definition.name + " expects a BIGINT");
        }
        value = Cell::make_int(static_cast<int64_t>(literal.number));
        break;
      case Literal::Kind::Str:
        if (definition.kind != ColKind::VarChar) {
          throw_error("type_mismatch", "column " + definition.name + " expects a string");
        }
        value = Cell::make_text(literal.text);
        break;
      case Literal::Kind::Vector:
        if (definition.kind != ColKind::Vector) {
          throw_error("invalid_vector", "column " + definition.name + " expects a string");
        }
        value = Cell::make_vector(literal.vector);
        break;
      case Literal::Kind::Null:
        throw_error("unsupported_sql", "NULL values are not supported");
    }
    columns.push_back(column);
    values.push_back(std::move(value));
  }
  std::vector<Predicate> predicates = resolve_predicates(statement, def);
  int64_t affected = table->update_rows(columns, values, predicates);
  refresh_catalog_entry(table);
  Result result;
  result.affected_rows = affected;
  return result;
}

Database::Result Database::execute_delete(const Statement& statement) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  std::vector<Predicate> predicates = resolve_predicates(statement, table->def());
  int64_t affected = table->delete_rows(predicates);
  refresh_catalog_entry(table);
  Result result;
  result.affected_rows = affected;
  return result;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

Database::Result Database::execute_select(const Statement& statement, bool ann_mode) {
  Table* table = find_table(statement.table_name);
  if (table == nullptr) {
    throw_error("table_not_found", "unknown table " + statement.table_name);
  }
  const TableDef& def = table->def();

  // Resolve the projection.
  struct Projection {
    enum class Kind { Column, Distance };
    Kind kind = Kind::Column;
    int column = -1;
    std::vector<float> query_vector;
    std::string name;
  };
  std::vector<Projection> projections;
  if (statement.select_star) {
    for (size_t i = 0; i < def.columns.size(); ++i) {
      Projection projection;
      projection.kind = Projection::Kind::Column;
      projection.column = static_cast<int>(i);
      projection.name = def.columns[i].name;
      projections.push_back(std::move(projection));
    }
  } else {
    for (const Expr& item : statement.select_items) {
      Projection projection;
      if (item.kind == Expr::Kind::ColumnRef) {
        int column = def.index_of(item.column_name);
        if (column < 0) {
          throw_error("column_not_found", "unknown column " + item.column_name);
        }
        projection.kind = Projection::Kind::Column;
        projection.column = column;
        projection.name = item.has_alias ? item.alias : def.columns[column].name;
      } else {
        int column = def.index_of(item.distance_column);
        if (column < 0) {
          throw_error("column_not_found", "unknown column " + item.distance_column);
        }
        if (def.columns[column].kind != ColKind::Vector) {
          throw_error("type_mismatch", "L2_DISTANCE requires a vector column");
        }
        if (item.distance_vector.size() != def.columns[column].length) {
          throw_error("dimension_mismatch",
                      "the query vector has " + std::to_string(item.distance_vector.size()) +
                          " components but the column has " +
                          std::to_string(def.columns[column].length));
        }
        projection.kind = Projection::Kind::Distance;
        projection.column = column;
        projection.query_vector = item.distance_vector;
        projection.name = item.has_alias ? item.alias : item.text;
      }
      projections.push_back(std::move(projection));
    }
  }

  std::vector<Predicate> predicates = resolve_predicates(statement, def);

  // Resolve the ORDER BY keys.
  struct OrderKey {
    // -1 means "the primary key", otherwise the index of the projection.
    int projection = -1;
    int column = -1;
    std::vector<float> query_vector;
    bool descending = false;
    bool is_distance = false;
  };
  std::vector<OrderKey> order_keys;
  for (const OrderItem& item : statement.order_by) {
    OrderKey key;
    key.descending = item.descending;
    if (item.expr.kind == Expr::Kind::Distance) {
      int column = def.index_of(item.expr.distance_column);
      if (column < 0) {
        throw_error("column_not_found", "unknown column " + item.expr.distance_column);
      }
      if (def.columns[column].kind != ColKind::Vector) {
        throw_error("type_mismatch", "L2_DISTANCE requires a vector column");
      }
      if (item.expr.distance_vector.size() != def.columns[column].length) {
        throw_error("dimension_mismatch",
                    "the query vector has " + std::to_string(item.expr.distance_vector.size()) +
                        " components but the column has " +
                        std::to_string(def.columns[column].length));
      }
      key.is_distance = true;
      key.column = column;
      key.query_vector = item.expr.distance_vector;
      // Prefer a projection that computes the same distance.
      for (size_t i = 0; i < projections.size(); ++i) {
        if (projections[i].kind == Projection::Kind::Distance &&
            projections[i].column == column &&
            projections[i].query_vector == item.expr.distance_vector) {
          key.projection = static_cast<int>(i);
          break;
        }
      }
    } else {
      int column = def.index_of(item.expr.column_name);
      // The name may refer to an output alias.
      bool matched = false;
      for (size_t i = 0; i < projections.size(); ++i) {
        if (projections[i].name == item.expr.column_name) {
          key.projection = static_cast<int>(i);
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (column < 0) {
          throw_error("column_not_found", "unknown column " + item.expr.column_name);
        }
        key.column = column;
      }
    }
    order_keys.push_back(std::move(key));
  }

  const int dimension = def.vector_index >= 0 ? static_cast<int>(def.columns[def.vector_index].length) : 0;

  // Collect the candidate rows. The sort keys live in one flat array, so that
  // a scan of ten thousand rows does not allocate ten thousand vectors.
  std::vector<Candidate> candidates;
  std::vector<SortKey> keys;
  std::string arena;  // holds the text of the ORDER BY keys
  bool used_ann = false;
  if (ann_mode && order_keys.size() == 1 && order_keys[0].is_distance &&
      table->vector_index() != nullptr && table->def().vector_index >= 0) {
    // Approximate path: ask the vector index for candidates and refine them.
    // The candidate budget starts small and is only raised when the filter
    // removes too many of the candidates, so that the common case stays cheap.
    const float* query = order_keys[0].query_vector.data();
    const int wanted = statement.has_limit ? static_cast<int>(statement.limit) : 10;
    const int total = static_cast<int>(table->row_count());
    int budget = std::max(4 * wanted, 64);
    if (budget > total) budget = total;
    std::vector<int64_t> accepted;
    std::vector<float> distances;
    std::vector<std::pair<int64_t, float>> hits;
    while (true) {
      hits.clear();
      if (budget > 0) {
        table->vector_index()->search(query, budget, hits, budget);
      }
      accepted.clear();
      distances.clear();
      // The candidates are filtered and measured page by page, so that every
      // page of the buffer pool is pinned once instead of once per candidate.
      table->filter_candidates(hits, predicates, query, dimension, &accepted, &distances);
      if (static_cast<int>(accepted.size()) >= wanted || budget >= total) break;
      budget = std::min(total, budget * 4);
    }
    for (size_t i = 0; i < accepted.size(); ++i) {
      Candidate candidate;
      candidate.pk = accepted[i];
      candidate.key_offset = static_cast<uint32_t>(keys.size());
      candidate.key_count = 1;
      SortKey key;
      key.kind = SortKey::Kind::Number;
      key.number = distances[i];
      keys.push_back(key);
      candidates.push_back(candidate);
    }
    bool enough = statement.has_limit
                      ? static_cast<int64_t>(candidates.size()) >= statement.limit
                      : !candidates.empty();
    if (enough) {
      used_ann = true;
    } else {
      candidates.clear();
      keys.clear();
    }
  }

  if (!used_ann) {
    // Exact path: a full scan with the filter applied before the ranking.
    Table::Scanner scanner(*table);
    RowView row;
    while (scanner.next(&row)) {
      if (!table->matches(row, predicates)) continue;
      Candidate candidate;
      candidate.pk = row.pk;
      candidate.page_id = row.page_id;
      candidate.slot_index = row.slot_index;
      candidate.key_offset = static_cast<uint32_t>(keys.size());
      candidate.key_count = static_cast<uint32_t>(order_keys.size());
      for (const OrderKey& key : order_keys) {
        SortKey sort_key;
        if (key.projection >= 0) {
          const Projection& projection = projections[key.projection];
          if (projection.kind == Projection::Kind::Distance) {
            sort_key.kind = SortKey::Kind::Number;
            sort_key.number = l2_distance(table->row_vector(row), projection.query_vector.data(),
                                          dimension);
          } else if (def.columns[projection.column].kind == ColKind::VarChar) {
            sort_key.kind = SortKey::Kind::Text;
            uint32_t length = 0;
            const char* text = table->row_text_view(row, projection.column, &length);
            sort_key.text_offset = static_cast<uint32_t>(arena.size());
            sort_key.text_length = length;
            arena.append(text, length);
          } else {
            sort_key.kind = SortKey::Kind::Integer;
            sort_key.integer = table->row_int(row, projection.column);
          }
        } else if (key.is_distance) {
          sort_key.kind = SortKey::Kind::Number;
          sort_key.number =
              l2_distance(table->row_vector(row), key.query_vector.data(), dimension);
        } else if (def.columns[key.column].kind == ColKind::VarChar) {
          sort_key.kind = SortKey::Kind::Text;
          uint32_t length = 0;
          const char* text = table->row_text_view(row, key.column, &length);
          sort_key.text_offset = static_cast<uint32_t>(arena.size());
          sort_key.text_length = length;
          arena.append(text, length);
        } else {
          sort_key.kind = SortKey::Kind::Integer;
          sort_key.integer = table->row_int(row, key.column);
        }
        keys.push_back(sort_key);
      }
      candidates.push_back(candidate);
    }
  }

  // Sort. Equal keys are ordered by the primary key, ascending.
  auto compare_candidates = [&](const Candidate& left, const Candidate& right) {
    for (uint32_t i = 0; i < left.key_count; ++i) {
      const SortKey& left_key = keys[left.key_offset + i];
      const SortKey& right_key = keys[right.key_offset + i];
      bool less = key_less(left_key, right_key, arena);
      bool greater = key_less(right_key, left_key, arena);
      if (less != greater) return order_keys[i].descending ? greater : less;
    }
    return left.pk < right.pk;
  };
  if (statement.has_limit && static_cast<int64_t>(candidates.size()) > statement.limit) {
    // Only the first LIMIT rows are needed, so the candidates are selected
    // instead of being sorted completely.
    size_t wanted = static_cast<size_t>(statement.limit);
    std::nth_element(candidates.begin(), candidates.begin() + wanted, candidates.end(),
                     compare_candidates);
    std::sort(candidates.begin(), candidates.begin() + wanted, compare_candidates);
    candidates.resize(wanted);
  } else {
    std::sort(candidates.begin(), candidates.end(), compare_candidates);
  }

  // Project the selected rows.
  Result result;
  result.query = true;
  for (const Projection& projection : projections) {
    result.columns.push_back(projection.name);
  }
  for (const Candidate& candidate : candidates) {
    RowView row;
    if (!table->find_by_pk(candidate.pk, &row)) continue;
    std::vector<Cell> cells;
    cells.reserve(projections.size());
    for (const Projection& projection : projections) {
      if (projection.kind == Projection::Kind::Distance) {
        cells.push_back(Cell::make_double(
            l2_distance(table->row_vector(row), projection.query_vector.data(), dimension)));
      } else if (def.columns[projection.column].kind == ColKind::VarChar) {
        cells.push_back(Cell::make_text(table->row_text(row, projection.column)));
      } else if (def.columns[projection.column].kind == ColKind::Vector) {
        const float* source = table->row_vector(row);
        cells.push_back(Cell::make_vector(
            std::vector<float>(source, source + def.columns[projection.column].length)));
      } else {
        cells.push_back(Cell::make_int(table->row_int(row, projection.column)));
      }
    }
    table->release_row(row);
    result.rows.push_back(std::move(cells));
  }
  return result;
}

}  // namespace mvdb
