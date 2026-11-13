// MiniVectorDB - database: catalog, transactions and statement execution.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core.hpp"
#include "sql.hpp"
#include "storage.hpp"
#include "table.hpp"

namespace mvdb {

class Database {
 public:
  explicit Database(std::string data_dir);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Opens the store, replays the write ahead log and rebuilds the catalog.
  void open();
  // Flushes and closes the store.
  void close();

  struct Result {
    bool query = false;
    std::vector<std::string> columns;
    std::vector<std::vector<Cell>> rows;
    int64_t affected_rows = 0;
  };

  // Executes one SQL statement. Throws DbException on failure; in that case
  // the statement left no partial changes behind.
  Result execute(const std::string& sql, bool ann_mode);

 private:
  // Catalog handling. The catalog itself is a table, stored as table id 0.
  static constexpr int64_t kCatalogId = 0;
  Table* catalog_table() { return catalog_.get(); }
  void bootstrap_catalog();
  void load_catalog();
  std::string serialize_def(const TableDef& def, uint64_t row_count) const;
  bool parse_def(const std::string& text, TableDef* def, uint64_t* row_count) const;
  void write_catalog_entry(const TableDef& def, uint64_t row_count);
  void refresh_catalog_entry(Table* table);

  Table* find_table(const std::string& name);
  Table* table_by_id(int64_t id);

  Result execute_statement(const Statement& statement, bool ann_mode);
  Result execute_create_table(const Statement& statement);
  Result execute_create_index(const Statement& statement);
  Result execute_drop_table(const Statement& statement);
  Result execute_insert(const Statement& statement);
  Result execute_select(const Statement& statement, bool ann_mode);
  Result execute_update(const Statement& statement);
  Result execute_delete(const Statement& statement);

  std::vector<Predicate> resolve_predicates(const Statement& statement, const TableDef& def);
  void validate_vector(const TableDef& def, int column, const std::vector<float>& values);

  Storage storage_;
  std::unique_ptr<Table> catalog_;
  std::vector<std::unique_ptr<Table>> tables_;
  bool explicit_txn_ = false;
  bool opened_ = false;
};

}  // namespace mvdb
