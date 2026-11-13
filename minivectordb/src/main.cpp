// MiniVectorDB - JSON Lines protocol front end.
//
// The judge drives the database over stdin and stdout: one JSON request per
// line, one JSON response per line. The startup handshake is a single
// {"type":"ready"} line that must be written and flushed before the first
// request is read.
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "core.hpp"
#include "db.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void on_signal(int) { g_stop_requested = 1; }

// Serialises a JSON value back into its compact text form.
void write_json_value(const mvdb::JsonValue& value, std::string& out) {
  using mvdb::JsonValue;
  switch (value.kind) {
    case JsonValue::Kind::Null: out += "null"; break;
    case JsonValue::Kind::Bool: out += value.boolean ? "true" : "false"; break;
    case JsonValue::Kind::Number: out += value.number; break;
    case JsonValue::Kind::String: mvdb::json_write_string(value.text, out); break;
    case JsonValue::Kind::Array: {
      out.push_back('[');
      for (size_t i = 0; i < value.array.size(); ++i) {
        if (i) out.push_back(',');
        write_json_value(value.array[i], out);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::Kind::Object: {
      out.push_back('{');
      for (size_t i = 0; i < value.members.size(); ++i) {
        if (i) out.push_back(',');
        mvdb::json_write_string(value.members[i].first, out);
        out.push_back(':');
        write_json_value(value.members[i].second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  const char* environment = std::getenv("VECTORDB_DATA_DIR");
  if (environment != nullptr && *environment != '\0') data_dir = environment;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      data_dir = argv[++i];
    }
  }

  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  mvdb::Database database(data_dir);
  int exit_code = 0;
  try {
    database.open();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "minivectordb: cannot open the database: %s\n", error.what());
    return 1;
  }

  // The handshake must be sent before the first request is read.
  std::fputs("{\"type\":\"ready\"}\n", stdout);
  std::fflush(stdout);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (g_stop_requested != 0) break;
    if (line.empty()) continue;

    std::string response;
    // The identifier is echoed verbatim so that it keeps its JSON type. It is
    // resolved before the request is executed, because a failing statement
    // still has to answer with the very same identifier.
    std::string id_text = "null";
    try {
      mvdb::JsonValue request = mvdb::json_parse(line);
      const mvdb::JsonValue* id_value = request.find("id");
      if (id_value != nullptr) {
        id_text.clear();
        write_json_value(*id_value, id_text);
      }

      const mvdb::JsonValue* op_value = request.find("op");
      std::string op;
      if (op_value != nullptr && op_value->kind == mvdb::JsonValue::Kind::String) {
        op = op_value->text;
      }

      if (op == "shutdown") {
        response = "{\"id\":" + id_text + ",\"ok\":true}";
        std::fputs(response.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        database.close();
        return 0;
      }
      if (op != "execute") {
        throw mvdb::DbException("unsupported_op", "unsupported operation: " + op);
      }

      const mvdb::JsonValue* sql_value = request.find("sql");
      if (sql_value == nullptr || sql_value->kind != mvdb::JsonValue::Kind::String) {
        throw mvdb::DbException("invalid_request", "the request has no sql field");
      }
      bool ann_mode = false;
      const mvdb::JsonValue* mode_value = request.find("search_mode");
      if (mode_value != nullptr && mode_value->kind == mvdb::JsonValue::Kind::String) {
        ann_mode = mode_value->text == "ann";
      }

      mvdb::Database::Result result = database.execute(sql_value->text, ann_mode);
      response = "{\"id\":" + id_text + ",\"ok\":true";
      if (result.query) {
        response += ",\"columns\":[";
        for (size_t i = 0; i < result.columns.size(); ++i) {
          if (i) response.push_back(',');
          mvdb::json_write_string(result.columns[i], response);
        }
        response += "],\"rows\":[";
        for (size_t i = 0; i < result.rows.size(); ++i) {
          if (i) response.push_back(',');
          response.push_back('[');
          for (size_t j = 0; j < result.rows[i].size(); ++j) {
            if (j) response.push_back(',');
            mvdb::json_write_cell(result.rows[i][j], response);
          }
          response.push_back(']');
        }
        response.push_back(']');
      } else {
        response += ",\"affected_rows\":";
        mvdb::json_write_int64(result.affected_rows, response);
      }
      response.push_back('}');
    } catch (const mvdb::DbException& error) {
      response = "{\"id\":" + id_text + ",\"ok\":false,\"error\":{\"code\":";
      mvdb::json_write_string(error.code(), response);
      response += ",\"message\":";
      mvdb::json_write_string(error.message(), response);
      response += "}}";
    } catch (const std::exception& error) {
      response = "{\"id\":" + id_text + ",\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":";
      mvdb::json_write_string(error.what(), response);
      response += "}}";
    }

    std::fputs(response.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  database.close();
  return exit_code;
}
