#include "lsp_server.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "grep_search.h"
#include "uri.h"

#include "json.hpp"

namespace slclangd::lsp {
using nlohmann::json;

namespace {

static std::string getStringOr(const json& j, const char* key, const std::string& def = {}) {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return def;
  return it->get<std::string>();
}

static int getIntOr(const json& j, const char* key, int def = 0) {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || !it->is_number_integer()) return def;
  return it->get<int>();
}

static std::string wordAt(const std::string& text, int line0, int ch0) {
  if (line0 < 0 || ch0 < 0) return {};
  int cur_line = 0;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      if (cur_line == line0) {
        std::size_t line_end = i;
        std::string_view line(text.data() + line_start, line_end - line_start);
        std::size_t c = static_cast<std::size_t>(std::min<int>(ch0, static_cast<int>(line.size())));

        auto is_word = [](unsigned char x) { return std::isalnum(x) || x == '_'; };
        std::size_t L = c;
        if (L > 0 && L == line.size()) L--;
        while (L > 0 && is_word(static_cast<unsigned char>(line[L])) == false && is_word(static_cast<unsigned char>(line[L - 1]))) {
          L--;
        }
        std::size_t start = L;
        while (start > 0 && is_word(static_cast<unsigned char>(line[start - 1]))) start--;
        std::size_t end = L;
        while (end < line.size() && is_word(static_cast<unsigned char>(line[end]))) end++;
        if (end <= start) return {};
        return std::string(line.substr(start, end - start));
      }
      cur_line++;
      line_start = i + 1;
    }
  }
  return {};
}

static json nullResult() { return nullptr; }

}  // namespace

Server::Server(Transport& transport, std::vector<std::string> serve_files)
    : transport_(transport), serve_files_(std::move(serve_files)) {
  const char* t1 = std::getenv("SLCLANGD_TRACE");
  const char* t2 = std::getenv("CLANGD_TRACE");  // used by vscode-clangd extension
  auto enabled = [](const char* v) {
    return v && *v && std::string(v) != "0";
  };
  trace_ = enabled(t1) || enabled(t2);
}

int Server::run() {
  while (!exit_requested_) {
    auto msg = transport_.readMessage();
    if (!msg.has_value()) break;
    if (msg->empty()) continue;
    handleMessage(*msg);
  }
  return shutdown_received_ ? 0 : 1;
}

void Server::handleMessage(const std::string& body) {
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    transport_.logLine("Failed to parse JSON: " + body);
    return;
  }

  const auto method_it = j.find("method");
  if (method_it == j.end() || !method_it->is_string()) {
    return;
  }
  std::string method = method_it->get<std::string>();
  if (trace_) transport_.logLine(std::string("LSP <= ") + method);
  json params = j.value("params", json::object());

  const auto id_it = j.find("id");
  if (id_it != j.end()) {
    handleRequest(method, *id_it, params);
  } else {
    handleNotification(method, params);
  }
}

void Server::handleRequest(const std::string& method, const json& id, const json& params) {
  try {
    if (method == "initialize") {
      replyResult(id, onInitialize(params));
      return;
    }
    if (method == "shutdown") {
      if (trace_) transport_.logLine("LSP <= shutdown");
      shutdown_received_ = true;
      replyResult(id, onShutdown());
      return;
    }
    // vscode-clangd may invoke commands for fix-its/tweaks. We don't implement them,
    // but returning null is better UX than "method not found".
    if (method == "workspace/executeCommand") {
      replyResult(id, nullResult());
      return;
    }
    // Command from vscode-clangd: Switch Between Source/Header.
    if (method == "textDocument/switchSourceHeader") {
      replyResult(id, nullResult());
      return;
    }
    if (method == "workspace/symbol") {
      replyResult(id, onWorkspaceSymbol(params));
      return;
    }
    if (method == "textDocument/hover") {
      replyResult(id, onHover(params));
      return;
    }
    if (method == "textDocument/definition") {
      replyResult(id, onDefinition(params));
      return;
    }
    if (method == "textDocument/references") {
      replyResult(id, onReferences(params));
      return;
    }

    replyError(id, -32601, "Method not found: " + method);
  } catch (const std::exception& e) {
    replyError(id, -32603, std::string("Internal error: ") + e.what());
  }
}

void Server::handleNotification(const std::string& method, const json& params) {
  if (method == "initialized") return onInitialized(params);
  if (method == "exit") return onExit();
  if (method == "$/setTrace") return;  // ignore
  if (method == "$/cancelRequest") return;  // ignore
  if (method == "workspace/didChangeConfiguration") return;  // ignore
  if (method == "textDocument/didOpen") return onDidOpen(params);
  if (method == "textDocument/didChange") return onDidChange(params);
  if (method == "textDocument/didClose") return onDidClose(params);
}

json Server::onInitialize(const json& params) {
  root_uri_ = getStringOr(params, "rootUri");
  root_path_ = getStringOr(params, "rootPath");
  if (root_path_.empty() && !root_uri_.empty()) root_path_ = fileUriToPath(root_uri_);
  if (root_uri_.empty() && !root_path_.empty()) root_uri_ = pathToFileUri(root_path_);

  // vscode-clangd sends initializationOptions: { clangdFileStatus: true, fallbackFlags: [...] }
  if (params.is_object()) {
    const auto it = params.find("initializationOptions");
    if (it != params.end() && it->is_object()) {
      const auto fs = it->find("clangdFileStatus");
      clangd_file_status_ = (fs != it->end() && fs->is_boolean() && fs->get<bool>());
    }
  }

  json caps;
  caps["textDocumentSync"] = json{
      {"openClose", true},
      {"change", 1},  // Full
  };
  caps["hoverProvider"] = true;
  caps["definitionProvider"] = true;
  caps["referencesProvider"] = true;
  caps["workspaceSymbolProvider"] = true;

  json out;
  out["capabilities"] = caps;
  out["serverInfo"] = json{{"name", "super-lazy-clangd"}, {"version", "0.1.0"}};
  return out;
}

void Server::onInitialized(const json&) {}

json Server::onShutdown() { return nullResult(); }

void Server::onExit() {
  // LSP: exit should terminate immediately; success depends on shutdown received.
  exit_requested_ = true;
}

void Server::onDidOpen(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  std::string text = getStringOr(td, "text");
  if (uri.empty()) return;
  docs_by_uri_[uri].text = std::move(text);
  if (clangd_file_status_) {
    sendNotification("textDocument/clangd.fileStatus", json{{"uri", uri}, {"state", "Idle"}});
  }
}

void Server::onDidChange(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return;

  auto changes = params.value("contentChanges", json::array());
  if (!changes.is_array() || changes.empty()) return;
  auto first = changes.at(0);
  std::string text = getStringOr(first, "text");
  docs_by_uri_[uri].text = std::move(text);
  if (clangd_file_status_) {
    sendNotification("textDocument/clangd.fileStatus", json{{"uri", uri}, {"state", "Idle"}});
  }
}

void Server::onDidClose(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return;
  docs_by_uri_.erase(uri);
}

std::string Server::rootDir() const {
  if (!root_path_.empty()) return root_path_;
  if (!root_uri_.empty()) return fileUriToPath(root_uri_);
  return ".";
}

std::string Server::makeResultPathAbsolute(const std::string& p) const {
  if (p.empty()) return p;
  try {
    std::filesystem::path path(p);
    if (path.is_absolute()) return path.lexically_normal().string();
    std::filesystem::path base(rootDir());
    return (base / path).lexically_normal().string();
  } catch (...) {
    return p;
  }
}

json Server::onWorkspaceSymbol(const json& params) {
  std::string query = getStringOr(params, "query");
  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, query, 50);
  } else {
    matches = grepFixedString(rootDir(), query, 50, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"));
  }

  json arr = json::array();
  for (const auto& m : matches) {
    std::string abs = makeResultPathAbsolute(m.path);
    json loc;
    loc["uri"] = pathToFileUri(abs);
    loc["range"] = json{
        {"start", json{{"line", m.line - 1}, {"character", m.column}}},
        {"end", json{{"line", m.line - 1}, {"character", m.column + static_cast<int>(query.size())}}},
    };

    json si;
    si["name"] = query;
    si["kind"] = 13;  // Variable (arbitrary; we're grep-based)
    si["location"] = loc;
    si["containerName"] = abs;
    arr.push_back(std::move(si));
  }
  return arr;
}

json Server::onHover(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return nullResult();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return nullResult();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (sym.empty()) return nullResult();

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 1);
  } else {
    matches = grepFixedString(rootDir(), sym, 1, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"));
  }
  if (matches.empty()) return nullResult();

  const auto& m = matches.front();
  std::string abs = makeResultPathAbsolute(m.path);
  json hover;
  hover["contents"] = json{
      {"kind", "markdown"},
      {"value", std::string("**super-lazy-clangd** (grep)\n\nFound `") + abs + ":" + std::to_string(m.line) + "`\n\n```cpp\n" +
                    m.text + "\n```"},
  };
  hover["range"] = json{
      {"start", json{{"line", line0}, {"character", ch0}}},
      {"end", json{{"line", line0}, {"character", ch0}}},
  };
  return hover;
}

json Server::onDefinition(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return nullResult();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return nullResult();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (sym.empty()) return nullResult();

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 20);
  } else {
    matches = grepFixedString(rootDir(), sym, 20, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"));
  }
  if (matches.empty()) return nullResult();

  json locs = json::array();
  for (const auto& m : matches) {
    std::string abs = makeResultPathAbsolute(m.path);
    json loc;
    loc["uri"] = pathToFileUri(abs);
    loc["range"] = json{
        {"start", json{{"line", m.line - 1}, {"character", m.column}}},
        {"end", json{{"line", m.line - 1}, {"character", m.column + static_cast<int>(sym.size())}}},
    };
    locs.push_back(std::move(loc));
  }
  return locs;
}

json Server::onReferences(const json& params) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return json::array();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return json::array();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (sym.empty()) return json::array();

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 50);
  } else {
    matches = grepFixedString(rootDir(), sym, 50, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"));
  }

  json locs = json::array();
  for (const auto& m : matches) {
    std::string abs = makeResultPathAbsolute(m.path);
    json loc;
    loc["uri"] = pathToFileUri(abs);
    loc["range"] = json{
        {"start", json{{"line", m.line - 1}, {"character", m.column}}},
        {"end", json{{"line", m.line - 1}, {"character", m.column + static_cast<int>(sym.size())}}},
    };
    locs.push_back(std::move(loc));
  }
  return locs;
}

void Server::replyResult(const json& id, const json& result) {
  json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  resp["result"] = result;
  transport_.writeMessage(resp.dump());
}

void Server::replyError(const json& id, int code, const std::string& message) {
  json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  resp["error"] = json{{"code", code}, {"message", message}};
  transport_.writeMessage(resp.dump());
}

void Server::sendNotification(const std::string& method, const json& params) {
  json msg;
  msg["jsonrpc"] = "2.0";
  msg["method"] = method;
  msg["params"] = params;
  transport_.writeMessage(msg.dump());
}

}  // namespace slclangd::lsp


