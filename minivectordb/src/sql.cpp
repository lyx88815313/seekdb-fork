#include "sql.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

namespace mvdb {

namespace {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class Tok { End, Identifier, Int, Float, String, Symbol };

struct Token {
  Tok kind = Tok::End;
  std::string text;    // identifier name / symbol / string payload
  int64_t integer = 0;
  double number = 0;
  size_t begin = 0;    // source offset of the first character
  size_t end = 0;      // source offset just past the token
};

class Lexer {
 public:
  explicit Lexer(const std::string& source) : source_(source) {}

  std::vector<Token> tokenize() {
    std::vector<Token> tokens;
    while (true) {
      Token token = next();
      tokens.push_back(token);
      if (token.kind == Tok::End) break;
    }
    return tokens;
  }

 private:
  const std::string& source_;
  size_t pos_ = 0;

  void skip_space() {
    while (pos_ < source_.size()) {
      unsigned char c = static_cast<unsigned char>(source_[pos_]);
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '-' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '-') {
        while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
      } else if (c == '#') {
        while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
      } else if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < source_.size() &&
               !(source_[pos_] == '*' && source_[pos_ + 1] == '/')) {
          ++pos_;
        }
        pos_ = (pos_ + 2 <= source_.size()) ? pos_ + 2 : source_.size();
      } else {
        return;
      }
    }
  }

  Token next() {
    skip_space();
    Token token;
    token.begin = pos_;
    if (pos_ >= source_.size()) {
      token.kind = Tok::End;
      token.end = pos_;
      return token;
    }
    char c = source_[pos_];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      size_t start = pos_;
      while (pos_ < source_.size()) {
        char d = source_[pos_];
        if (std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '$') {
          ++pos_;
        } else {
          break;
        }
      }
      token.kind = Tok::Identifier;
      token.text = source_.substr(start, pos_ - start);
      for (char& ch : token.text) ch = static_cast<char>(std::tolower(ch));
      token.end = pos_;
      return token;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && pos_ + 1 < source_.size() &&
         std::isdigit(static_cast<unsigned char>(source_[pos_ + 1])))) {
      size_t start = pos_;
      bool is_float = false;
      while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) ++pos_;
      if (pos_ < source_.size() && source_[pos_] == '.') {
        is_float = true;
        ++pos_;
        while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) ++pos_;
      }
      if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
        is_float = true;
        ++pos_;
        if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) ++pos_;
        while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) ++pos_;
      }
      std::string text = source_.substr(start, pos_ - start);
      token.text = text;
      if (is_float) {
        token.kind = Tok::Float;
        token.number = std::strtod(text.c_str(), nullptr);
      } else {
        token.kind = Tok::Int;
        errno = 0;
        char* end = nullptr;
        token.integer = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE || (end && *end != '\0')) {
          throw_error("value_out_of_range", "integer literal does not fit in BIGINT: " + text);
        }
      }
      token.end = pos_;
      return token;
    }
    if (c == '\'' || c == '"') {
      char quote = c;
      ++pos_;
      std::string text;
      while (true) {
        if (pos_ >= source_.size()) {
          throw_error("syntax_error", "unterminated string literal");
        }
        char d = source_[pos_];
        if (d == quote) {
          if (pos_ + 1 < source_.size() && source_[pos_ + 1] == quote) {
            text.push_back(quote);
            pos_ += 2;
            continue;
          }
          ++pos_;
          break;
        }
        text.push_back(d);
        ++pos_;
      }
      token.kind = Tok::String;
      token.text = std::move(text);
      token.end = pos_;
      return token;
    }
    if (pos_ + 1 < source_.size()) {
      std::string two = source_.substr(pos_, 2);
      if (two == "<=" || two == ">=" || two == "!=" || two == "<>") {
        pos_ += 2;
        token.kind = Tok::Symbol;
        token.text = two;
        token.end = pos_;
        return token;
      }
    }
    ++pos_;
    token.kind = Tok::Symbol;
    token.text = std::string(1, c);
    token.end = pos_;
    return token;
  }
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
 public:
  explicit Parser(const std::string& sql) : sql_(sql) {
    Lexer lexer(sql);
    tokens_ = lexer.tokenize();
  }

  Statement parse() {
    if (peek().kind == Tok::End) {
      throw_error("syntax_error", "empty statement");
    }
    Statement statement;
    std::string keyword = expect_keyword();
    if (keyword == "create") {
      statement = parse_create();
    } else if (keyword == "insert") {
      statement = parse_insert();
    } else if (keyword == "select") {
      statement = parse_select();
    } else if (keyword == "update") {
      statement = parse_update();
    } else if (keyword == "delete") {
      statement = parse_delete();
    } else if (keyword == "drop") {
      statement = parse_drop();
    } else if (keyword == "begin") {
      statement.kind = StmtKind::Begin;
    } else if (keyword == "commit") {
      statement.kind = StmtKind::Commit;
    } else if (keyword == "rollback") {
      statement.kind = StmtKind::Rollback;
    } else {
      throw_error("unsupported_sql", "unsupported statement: " + keyword);
    }
    expect_end();
    return statement;
  }

 private:
  const std::string& sql_;
  std::vector<Token> tokens_;
  size_t pos_ = 0;

  const Token& peek(size_t ahead = 0) const {
    size_t index = pos_ + ahead;
    if (index >= tokens_.size()) index = tokens_.size() - 1;
    return tokens_[index];
  }

  void advance() {
    if (pos_ + 1 < tokens_.size()) ++pos_;
  }

  bool is_keyword(const char* word, size_t ahead = 0) const {
    const Token& token = peek(ahead);
    return token.kind == Tok::Identifier && token.text == word;
  }

  bool is_symbol(const char* symbol, size_t ahead = 0) const {
    const Token& token = peek(ahead);
    return token.kind == Tok::Symbol && token.text == symbol;
  }

  std::string expect_keyword() {
    const Token& token = peek();
    if (token.kind != Tok::Identifier) {
      throw_error("syntax_error", "expected a statement keyword");
    }
    advance();
    return token.text;
  }

  void expect_symbol(const char* symbol) {
    if (!is_symbol(symbol)) {
      throw_error("syntax_error", std::string("expected '") + symbol + "'");
    }
    advance();
  }

  std::string expect_identifier() {
    const Token& token = peek();
    if (token.kind != Tok::Identifier) {
      throw_error("syntax_error", "expected an identifier");
    }
    advance();
    return token.text;
  }

  void expect_end() {
    while (is_symbol(";")) advance();
    if (peek().kind != Tok::End) {
      throw_error("syntax_error", "unexpected trailing input");
    }
  }

  [[noreturn]] void reject_unsupported(const std::string& what) {
    throw_error("unsupported_sql", "unsupported SQL feature: " + what);
  }

  Literal parse_literal() {
    Literal literal;
    const Token& token = peek();
    if (token.kind == Tok::Int) {
      advance();
      literal.kind = Literal::Kind::Int;
      literal.integer = token.integer;
      return literal;
    }
    if (token.kind == Tok::Float) {
      advance();
      literal.kind = Literal::Kind::Double;
      literal.number = token.number;
      return literal;
    }
    if (token.kind == Tok::String) {
      advance();
      literal.kind = Literal::Kind::Str;
      literal.text = token.text;
      return literal;
    }
    if (token.kind == Tok::Identifier && token.text == "null") {
      advance();
      literal.kind = Literal::Kind::Null;
      return literal;
    }
    if (token.kind == Tok::Symbol && token.text == "-") {
      advance();
      const Token& number = peek();
      if (number.kind == Tok::Int) {
        advance();
        literal.kind = Literal::Kind::Int;
        literal.integer = -number.integer;
        return literal;
      }
      if (number.kind == Tok::Float) {
        advance();
        literal.kind = Literal::Kind::Double;
        literal.number = -number.number;
        return literal;
      }
    }
    throw_error("syntax_error", "expected a literal value");
  }

  // A literal that may be a vector, i.e. a string holding "[...]".
  Literal parse_value_literal() {
    Literal literal = parse_literal();
    if (literal.kind == Literal::Kind::Str && !literal.text.empty() &&
        literal.text[0] == '[') {
      literal.vector = parse_vector_literal(literal.text);
      literal.kind = Literal::Kind::Vector;
      literal.text.clear();
    }
    return literal;
  }

  Statement parse_create() {
    Statement statement;
    if (is_keyword("table")) {
      advance();
      statement.kind = StmtKind::CreateTable;
      statement.table_name = expect_identifier();
      expect_symbol("(");
      while (true) {
        ColumnDef column;
        column.name = expect_identifier();
        std::string type = expect_keyword();
        if (type == "bigint") {
          column.kind = ColKind::BigInt;
        } else if (type == "varchar") {
          column.kind = ColKind::VarChar;
          expect_symbol("(");
          const Token& length = peek();
          if (length.kind != Tok::Int) {
            throw_error("syntax_error", "VARCHAR requires an integer length");
          }
          advance();
          if (length.integer < 0) throw_error("syntax_error", "VARCHAR length must not be negative");
          column.length = static_cast<uint32_t>(length.integer);
          expect_symbol(")");
        } else if (type == "vector") {
          column.kind = ColKind::Vector;
          expect_symbol("(");
          const Token& dimension = peek();
          if (dimension.kind != Tok::Int) {
            throw_error("syntax_error", "VECTOR requires an integer dimension");
          }
          advance();
          if (dimension.integer <= 0) {
            throw_error("syntax_error", "VECTOR dimension must be positive");
          }
          column.length = static_cast<uint32_t>(dimension.integer);
          expect_symbol(")");
        } else {
          reject_unsupported("column type " + type);
        }
        if (is_keyword("primary")) {
          advance();
          std::string key = expect_keyword();
          if (key != "key") throw_error("syntax_error", "expected PRIMARY KEY");
          column.primary_key = true;
        }
        if (is_keyword("not")) {
          advance();
          std::string null_word = expect_keyword();
          if (null_word != "null") throw_error("syntax_error", "expected NOT NULL");
          reject_unsupported("NOT NULL constraints");
        }
        statement.columns.push_back(std::move(column));
        if (is_symbol(",")) {
          advance();
          continue;
        }
        break;
      }
      expect_symbol(")");
      return statement;
    }
    if (is_keyword("vector")) {
      advance();
      std::string index_word = expect_keyword();
      if (index_word != "index") {
        throw_error("syntax_error", "expected CREATE VECTOR INDEX");
      }
      statement.kind = StmtKind::CreateVectorIndex;
      statement.index_name = expect_identifier();
      if (!is_keyword("on")) throw_error("syntax_error", "expected ON");
      advance();
      statement.table_name = expect_identifier();
      expect_symbol("(");
      statement.index_column_name = expect_identifier();
      expect_symbol(")");
      return statement;
    }
    reject_unsupported("CREATE statement");
  }

  Statement parse_drop() {
    std::string what = expect_keyword();
    if (what != "table") {
      reject_unsupported("DROP " + what);
    }
    Statement statement;
    statement.kind = StmtKind::DropTable;
    statement.table_name = expect_identifier();
    return statement;
  }

  Statement parse_insert() {
    Statement statement;
    statement.kind = StmtKind::Insert;
    if (!is_keyword("into")) throw_error("syntax_error", "expected INTO");
    advance();
    statement.table_name = expect_identifier();
    if (is_symbol("(")) {
      advance();
      while (true) {
        statement.insert_column_names.push_back(expect_identifier());
        if (is_symbol(",")) {
          advance();
          continue;
        }
        break;
      }
      expect_symbol(")");
    }
    if (!is_keyword("values")) throw_error("syntax_error", "expected VALUES");
    advance();
    while (true) {
      expect_symbol("(");
      std::vector<Literal> row;
      while (true) {
        row.push_back(parse_value_literal());
        if (is_symbol(",")) {
          advance();
          continue;
        }
        break;
      }
      expect_symbol(")");
      statement.insert_rows.push_back(std::move(row));
      if (is_symbol(",")) {
        advance();
        continue;
      }
      break;
    }
    return statement;
  }

  Statement parse_select() {
    Statement statement;
    statement.kind = StmtKind::Select;
    if (is_symbol("*")) {
      advance();
      statement.select_star = true;
    } else {
      while (true) {
        statement.select_items.push_back(parse_select_item());
        if (is_symbol(",")) {
          advance();
          continue;
        }
        break;
      }
    }
    if (!is_keyword("from")) throw_error("syntax_error", "expected FROM");
    advance();
    if (is_symbol("(")) reject_unsupported("subqueries");
    statement.table_name = expect_identifier();
    if (is_symbol(",")) reject_unsupported("joins");
    if (is_keyword("join") || is_keyword("inner") || is_keyword("left") ||
        is_keyword("right") || is_keyword("cross") || is_keyword("outer")) {
      reject_unsupported("joins");
    }
    if (is_keyword("where")) {
      advance();
      parse_where(&statement);
    }
    if (is_keyword("group") || is_keyword("having") || is_keyword("union") ||
        is_keyword("distinct") || is_keyword("window")) {
      reject_unsupported("grouping, set operations and DISTINCT");
    }
    if (is_keyword("order")) {
      advance();
      if (!is_keyword("by")) throw_error("syntax_error", "expected BY");
      advance();
      while (true) {
        OrderItem item;
        item.expr = parse_order_expr();
        if (is_keyword("asc")) {
          advance();
        } else if (is_keyword("desc")) {
          advance();
          item.descending = true;
        }
        statement.order_by.push_back(std::move(item));
        if (is_symbol(",")) {
          advance();
          continue;
        }
        break;
      }
    }
    if (is_keyword("limit")) {
      advance();
      const Token& limit = peek();
      if (limit.kind != Tok::Int) throw_error("syntax_error", "LIMIT requires an integer");
      advance();
      if (limit.integer < 0) throw_error("syntax_error", "LIMIT must not be negative");
      statement.has_limit = true;
      statement.limit = limit.integer;
      if (is_symbol(",")) reject_unsupported("LIMIT offset syntax");
      if (is_keyword("offset")) reject_unsupported("LIMIT offset syntax");
    }
    return statement;
  }

  // A projection item: a column reference, or L2_DISTANCE(vector_col, literal).
  Expr parse_select_item() {
    Expr expr;
    size_t start = peek().begin;
    if (is_keyword("l2_distance")) {
      advance();
      expect_symbol("(");
      expr.kind = Expr::Kind::Distance;
      expr.distance_column = expect_identifier();
      expect_symbol(",");
      Literal literal = parse_value_literal();
      if (literal.kind != Literal::Kind::Vector) {
        throw_error("invalid_vector", "L2_DISTANCE requires a vector literal");
      }
      expr.distance_vector = std::move(literal.vector);
      expect_symbol(")");
      expr.text = sql_.substr(start, peek().end - start);
      while (!expr.text.empty() && expr.text.back() == ' ') expr.text.pop_back();
    } else {
      expr.kind = Expr::Kind::ColumnRef;
      expr.column_name = expect_identifier();
      expr.text = expr.column_name;
    }
    if (is_keyword("as")) {
      advance();
      expr.alias = expect_identifier();
      expr.has_alias = true;
    }
    return expr;
  }

  // ORDER BY may reference a column, an output alias, or an expression.
  Expr parse_order_expr() {
    if (is_keyword("l2_distance")) {
      return parse_select_item();
    }
    Expr expr;
    expr.kind = Expr::Kind::ColumnRef;
    expr.column_name = expect_identifier();
    expr.text = expr.column_name;
    return expr;
  }

  void parse_where(Statement* statement) {
    while (true) {
      Compare compare;
      compare.column_name = expect_identifier();
      const Token& op = peek();
      if (op.kind != Tok::Symbol) throw_error("syntax_error", "expected a comparison operator");
      std::string symbol = op.text;
      if (symbol == "=") {
        compare.op = Compare::Op::Eq;
      } else if (symbol == "<") {
        compare.op = Compare::Op::Lt;
      } else if (symbol == "<=") {
        compare.op = Compare::Op::Le;
      } else if (symbol == ">") {
        compare.op = Compare::Op::Gt;
      } else if (symbol == ">=") {
        compare.op = Compare::Op::Ge;
      } else {
        reject_unsupported("comparison operator " + symbol);
      }
      advance();
      compare.value = parse_value_literal();
      statement->where.push_back(std::move(compare));
      if (is_keyword("and")) {
        advance();
        continue;
      }
      if (is_keyword("or")) reject_unsupported("OR in WHERE");
      break;
    }
  }

  Statement parse_update() {
    Statement statement;
    statement.kind = StmtKind::Update;
    statement.table_name = expect_identifier();
    if (!is_keyword("set")) throw_error("syntax_error", "expected SET");
    advance();
    while (true) {
      std::string column_name = expect_identifier();
      expect_symbol("=");
      Literal literal = parse_value_literal();
      statement.assignments.emplace_back(std::move(column_name), std::move(literal));
      if (is_symbol(",")) {
        advance();
        continue;
      }
      break;
    }
    if (is_keyword("where")) {
      advance();
      parse_where(&statement);
    }
    return statement;
  }

  Statement parse_delete() {
    Statement statement;
    statement.kind = StmtKind::Delete;
    if (!is_keyword("from")) throw_error("syntax_error", "expected FROM");
    advance();
    statement.table_name = expect_identifier();
    if (is_keyword("where")) {
      advance();
      parse_where(&statement);
    }
    return statement;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Vector literals
// ---------------------------------------------------------------------------

std::vector<float> parse_vector_literal(const std::string& text) {
  std::vector<float> values;
  size_t pos = 0;
  auto skip = [&]() {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
      ++pos;
    }
  };
  skip();
  if (pos >= text.size() || text[pos] != '[') {
    throw_error("invalid_vector", "vector literal must be enclosed in brackets");
  }
  ++pos;
  skip();
  if (pos < text.size() && text[pos] == ']') {
    ++pos;
    skip();
    if (pos != text.size()) {
      throw_error("invalid_vector", "trailing characters after vector literal");
    }
    return values;
  }
  while (true) {
    skip();
    size_t start = pos;
    while (pos < text.size() && text[pos] != ',' && text[pos] != ']') ++pos;
    std::string item = text.substr(start, pos - start);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t' ||
                             item.back() == '\n' || item.back() == '\r')) {
      item.pop_back();
    }
    size_t begin = 0;
    while (begin < item.size() && (item[begin] == ' ' || item[begin] == '\t')) ++begin;
    item = item.substr(begin);
    if (item.empty()) {
      throw_error("invalid_vector", "empty component in vector literal");
    }
    errno = 0;
    char* end = nullptr;
    double value = std::strtod(item.c_str(), &end);
    if (end == item.c_str() || (end && *end != '\0')) {
      throw_error("invalid_vector", "component is not a number: " + item);
    }
    if (!std::isfinite(value)) {
      throw_error("invalid_vector", "vector component is not finite: " + item);
    }
    float narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed)) {
      throw_error("invalid_vector", "vector component overflows float32: " + item);
    }
    values.push_back(narrowed);
    skip();
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < text.size() && text[pos] == ']') {
      ++pos;
      break;
    }
    throw_error("invalid_vector", "malformed vector literal");
  }
  skip();
  if (pos != text.size()) {
    throw_error("invalid_vector", "trailing characters after vector literal");
  }
  return values;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

Statement parse_sql(const std::string& sql) {
  Parser parser(sql);
  return parser.parse();
}

}  // namespace mvdb
