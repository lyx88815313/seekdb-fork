// MiniVectorDB - SQL subset: lexer, abstract syntax tree and parser.
#pragma once

#include <string>
#include <vector>

#include "core.hpp"

namespace mvdb {

// ---------------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------------

struct Literal {
  enum class Kind { Int, Double, Str, Vector, Null };
  Kind kind = Kind::Null;
  int64_t integer = 0;
  double number = 0;
  std::string text;
  std::vector<float> vector;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

// Columns are carried by name through the parser and resolved against the
// table schema by the executor.
struct Expr {
  enum class Kind { Literal, ColumnRef, Distance };
  Kind kind = Kind::Literal;
  Literal literal;
  std::string column_name;       // ColumnRef
  std::string distance_column;   // Distance: vector column name
  std::vector<float> distance_vector;
  std::string text;              // raw source text, the default output name
  std::string alias;
  bool has_alias = false;
};

struct Compare {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  std::string column_name;
  Op op = Op::Eq;
  Literal value;
};

struct OrderItem {
  Expr expr;
  bool descending = false;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind {
  CreateTable,
  CreateVectorIndex,
  DropTable,
  Insert,
  Select,
  Update,
  Delete,
  Begin,
  Commit,
  Rollback,
};

struct Statement {
  StmtKind kind = StmtKind::Select;

  std::string table_name;              // CREATE / INSERT / UPDATE / DELETE
  std::vector<ColumnDef> columns;      // CREATE TABLE
  std::string index_name;              // CREATE VECTOR INDEX
  std::string index_column_name;       // CREATE VECTOR INDEX

  std::vector<std::string> insert_column_names;        // INSERT target columns
  std::vector<std::vector<Literal>> insert_rows;

  std::vector<Expr> select_items;      // SELECT projection
  bool select_star = false;
  std::vector<Compare> where;          // WHERE conjuncts
  std::vector<OrderItem> order_by;     // ORDER BY keys
  bool has_limit = false;
  int64_t limit = 0;

  std::vector<std::pair<std::string, Literal>> assignments;  // UPDATE SET list
};

// Parses one SQL statement. Throws DbException with code "syntax_error" for
// malformed input and "unsupported_sql" for statements outside the subset
// described by the problem statement.
Statement parse_sql(const std::string& sql);

// Parses a vector literal such as "[1, 0.5, -2]". Throws DbException with
// code "invalid_vector" for non-finite or float32-overflowing components.
std::vector<float> parse_vector_literal(const std::string& text);

}  // namespace mvdb
