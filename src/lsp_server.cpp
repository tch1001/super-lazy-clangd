#include "lsp_server.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <string>
#include <thread>
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

static bool isInLineCommentAt(const std::string& text, int line0, int ch0) {
  if (line0 < 0 || ch0 < 0) return false;
  int cur_line = 0;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      if (cur_line == line0) {
        std::size_t line_end = i;
        std::string_view line(text.data() + line_start, line_end - line_start);
        std::size_t col = static_cast<std::size_t>(std::min<int>(ch0, static_cast<int>(line.size())));

        auto is_escaped_quote = [&](std::size_t pos) -> bool {
          std::size_t bs = 0;
          while (pos > 0 && line[pos - 1] == '\\') {
            ++bs;
            --pos;
          }
          return (bs % 2) == 1;
        };

        bool in_string = false;
        for (std::size_t j = 0; j + 1 < line.size(); ++j) {
          if (line[j] == '"' && !is_escaped_quote(j)) in_string = !in_string;
          if (!in_string && line[j] == '/' && line[j + 1] == '/') {
            return col >= j;
          }
        }
        return false;
      }
      cur_line++;
      line_start = i + 1;
    }
  }
  return false;
}

static bool isStopWord(std::string_view sym) {
  if (sym.empty()) return true;
  std::string lower;
  lower.reserve(sym.size());
  for (unsigned char c : sym) lower.push_back(static_cast<char>(std::tolower(c)));

  static const std::unordered_set<std::string> kStop = {
      // very common C/C++ keywords that shouldn't trigger grep
      "alignas",   "alignof",   "asm",      "auto",     "bool",     "break",   "case",   "catch",
      "char",      "char8_t",   "char16_t", "char32_t", "class",    "concept", "const",  "consteval",
      "constexpr", "constinit", "continue", "co_await", "co_return","co_yield","decltype","default",
      "delete",    "do",        "double",   "dynamic_cast","else",  "enum",    "explicit","export",
      "extern",    "false",     "float",    "for",      "friend",   "goto",    "if",     "inline",
      "int",       "long",      "mutable",  "namespace","new",      "noexcept","nullptr","operator",
      "private",   "protected", "public",   "register", "reinterpret_cast","requires","return",
      "short",     "signed",    "sizeof",   "static",   "static_assert","static_cast","struct",
      "switch",    "template",  "this",     "thread_local","throw", "true",    "try",    "typedef",
      "typeid",    "typename",  "union",    "unsigned", "using",    "virtual", "void",   "volatile",
      "wchar_t",   "while",
  };
  return kStop.find(lower) != kStop.end();
}

struct MatchRank {
  GrepMatch m;
  int score = 0;
  std::string abs_path;
};

static bool isWsOrBolBefore(const std::string& line, int col0) {
  if (col0 <= 0) return true;
  char c = line[static_cast<std::size_t>(col0 - 1)];
  return c == ' ' || c == '\t';
}

static std::optional<int> macroNameStartIfDefine(const std::string& line) {
  std::size_t i = 0;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i >= line.size() || line[i] != '#') return std::nullopt;
  ++i;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  constexpr std::string_view kDefine = "define";
  if (i + kDefine.size() > line.size()) return std::nullopt;
  if (std::string_view(line).substr(i, kDefine.size()) != kDefine) return std::nullopt;
  i += kDefine.size();
  if (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) return std::nullopt;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i >= line.size()) return std::nullopt;
  return static_cast<int>(i);
}

static int scoreMatchLine(const std::string& line, int col0, std::string_view needle) {
  if (col0 < 0) return -100000;
  int score = 0;

  auto prevNonSpace = [&](int before) -> char {
    int k = before;
    while (k > 0) {
      char c = line[static_cast<std::size_t>(k - 1)];
      if (c != ' ' && c != '\t') return c;
      --k;
    }
    return '\0';
  };

  auto prevIdentifier = [&](int before) -> std::string {
    // Walk left: skip whitespace, then common punctuation, then collect identifier.
    int k = before;
    while (k > 0 && (line[static_cast<std::size_t>(k - 1)] == ' ' || line[static_cast<std::size_t>(k - 1)] == '\t')) --k;
    while (k > 0) {
      char c = line[static_cast<std::size_t>(k - 1)];
      if (c == '*' || c == '&' || c == ':' || c == '<' || c == '>' || c == ',' || c == '(') {
        --k;
        continue;
      }
      break;
    }
    while (k > 0 && (line[static_cast<std::size_t>(k - 1)] == ' ' || line[static_cast<std::size_t>(k - 1)] == '\t')) --k;

    int end = k;
    while (k > 0) {
      unsigned char uc = static_cast<unsigned char>(line[static_cast<std::size_t>(k - 1)]);
      if (std::isalnum(uc) || uc == '_') {
        --k;
        continue;
      }
      break;
    }
    if (end <= k) return {};
    std::string tok = line.substr(static_cast<std::size_t>(k), static_cast<std::size_t>(end - k));
    for (auto& ch : tok) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return tok;
  };

  // Strong signal: macro definition (#define <needle> ...)
  if (auto macro_start = macroNameStartIfDefine(line)) {
    if (*macro_start == col0) score += 100;
  }

  // Boundary before token indicates likely declaration/definition site.
  if (isWsOrBolBefore(line, col0)) score += 25;

  // Template-ish / qualified type-ish: previous non-space is '>' (e.g. vector<T> foo(...))
  if (prevNonSpace(col0) == '>') score += 20;

  int end = col0 + static_cast<int>(needle.size());
  if (end < 0) end = 0;
  if (end > static_cast<int>(line.size())) end = static_cast<int>(line.size());

  // Lookahead after token.
  // - immediate ';' after token: very likely a declaration (e.g. "int foo;")
  if (end < static_cast<int>(line.size()) && line[static_cast<std::size_t>(end)] == ';') score += 40;

  // - next non-space is '(' : function-like (decl/def/call), still a good signal.
  std::size_t j = static_cast<std::size_t>(end);
  while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
  if (j < line.size() && line[j] == '(') score += 60;

  // If it's function-like and preceded by a primitive return type, boost more.
  // Heuristic examples: "int foo(", "void bar(", "unsigned long baz(".
  if (j < line.size() && line[j] == '(') {
    std::string prev = prevIdentifier(col0);
    static const std::unordered_set<std::string> kPrim = {
        "void", "bool", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned",
        "wchar_t", "char8_t", "char16_t", "char32_t",
        "size_t", "ssize_t",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
        "intptr_t", "uintptr_t",
        // Common kernel typedefs
        "u8", "u16", "u32", "u64",
        "s8", "s16", "s32", "s64",
    };
    if (!prev.empty() && kPrim.find(prev) != kPrim.end()) score += 30;
  }

  return score;
}

static std::vector<MatchRank> rankAndFilterMatches(const std::vector<GrepMatch>& matches,
                                                   std::string_view needle,
                                                   const std::string& current_abs_path,
                                                   int current_line1,
                                                   const std::string& prefer_abs_path,
                                                   const std::function<std::string(const std::string&)>& make_abs) {
  std::vector<MatchRank> out;
  out.reserve(matches.size());
  for (const auto& m : matches) {
    MatchRank r;
    r.m = m;
    r.abs_path = make_abs(m.path);
    if (!current_abs_path.empty() && current_line1 > 0) {
      if (r.abs_path == current_abs_path && m.line == current_line1) {
        continue;  // ignore exact same line; user is already there
      }
    }
    r.score = scoreMatchLine(m.text, m.column, needle);
    // Prefer matches in the same file as the query (useful for references),
    // but do not outrank real "definition-like" signals.
    if (!prefer_abs_path.empty() && r.abs_path == prefer_abs_path) r.score += 10;
    out.push_back(std::move(r));
  }
  std::stable_sort(out.begin(), out.end(), [](const MatchRank& a, const MatchRank& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.abs_path != b.abs_path) return a.abs_path < b.abs_path;
    if (a.m.line != b.m.line) return a.m.line < b.m.line;
    return a.m.column < b.m.column;
  });
  return out;
}

static json nullResult() { return nullptr; }

static std::string inflightKey(const json& id) {
  // Stable key for numeric/string ids.
  return id.dump();
}

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
      // Potentially slow: run async so $/cancelRequest can be processed.
      auto inflight = std::make_shared<InFlight>();
      {
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_[inflightKey(id)] = inflight;
      }
      std::thread([this, id, params, inflight]() {
        json result = nullptr;
        try {
          result = onWorkspaceSymbol(params, &inflight->cancelled, &inflight->grep_pid);
          if (inflight->cancelled.load(std::memory_order_acquire)) {
            replyError(id, -32800, "Request cancelled");
          } else {
            replyResult(id, result);
          }
        } catch (const std::exception& e) {
          replyError(id, -32603, std::string("Internal error: ") + e.what());
        }
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_.erase(inflightKey(id));
      }).detach();
      return;
    }
    if (method == "textDocument/hover") {
      auto inflight = std::make_shared<InFlight>();
      {
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_[inflightKey(id)] = inflight;
      }
      std::thread([this, id, params, inflight]() {
        json result = nullptr;
        try {
          result = onHover(params, &inflight->cancelled, &inflight->grep_pid);
          if (inflight->cancelled.load(std::memory_order_acquire)) {
            replyError(id, -32800, "Request cancelled");
          } else {
            replyResult(id, result);
          }
        } catch (const std::exception& e) {
          replyError(id, -32603, std::string("Internal error: ") + e.what());
        }
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_.erase(inflightKey(id));
      }).detach();
      return;
    }
    if (method == "textDocument/definition") {
      auto inflight = std::make_shared<InFlight>();
      {
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_[inflightKey(id)] = inflight;
      }
      std::thread([this, id, params, inflight]() {
        json result = nullptr;
        try {
          result = onDefinition(params, &inflight->cancelled, &inflight->grep_pid);
          if (inflight->cancelled.load(std::memory_order_acquire)) {
            replyError(id, -32800, "Request cancelled");
          } else {
            replyResult(id, result);
          }
        } catch (const std::exception& e) {
          replyError(id, -32603, std::string("Internal error: ") + e.what());
        }
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_.erase(inflightKey(id));
      }).detach();
      return;
    }
    if (method == "textDocument/references") {
      auto inflight = std::make_shared<InFlight>();
      {
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_[inflightKey(id)] = inflight;
      }
      std::thread([this, id, params, inflight]() {
        json result = nullptr;
        try {
          result = onReferences(params, &inflight->cancelled, &inflight->grep_pid);
          if (inflight->cancelled.load(std::memory_order_acquire)) {
            replyError(id, -32800, "Request cancelled");
          } else {
            replyResult(id, result);
          }
        } catch (const std::exception& e) {
          replyError(id, -32603, std::string("Internal error: ") + e.what());
        }
        std::lock_guard<std::mutex> lg(inflight_mu_);
        inflight_.erase(inflightKey(id));
      }).detach();
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
  if (method == "$/cancelRequest") {
    // LSP CancelParams: { id: number|string }
    if (!params.is_object()) return;
    auto it = params.find("id");
    if (it == params.end()) return;
    std::shared_ptr<InFlight> inflight;
    {
      std::lock_guard<std::mutex> lg(inflight_mu_);
      auto hit = inflight_.find(inflightKey(*it));
      if (hit != inflight_.end()) inflight = hit->second;
    }
    if (!inflight) return;
    inflight->cancelled.store(true, std::memory_order_release);
    pid_t pid = inflight->grep_pid.load(std::memory_order_acquire);
    if (pid > 0) {
      (void)kill(pid, SIGTERM);
    }
    return;
  }
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

json Server::onWorkspaceSymbol(const json& params, std::atomic_bool* cancelled, std::atomic<pid_t>* child_pid) {
  std::string query = getStringOr(params, "query");
  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, query, 50, cancelled, child_pid);
  } else {
    matches = grepFixedString(rootDir(), query, 50, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"), cancelled, child_pid);
  }

  // Rank likely declarations/definitions/macros higher.
  auto ranked =
      rankAndFilterMatches(matches, query, /*current_abs_path=*/"", /*current_line1=*/0, /*prefer_abs_path=*/"",
                           [this](const std::string& p) { return makeResultPathAbsolute(p); });

  json arr = json::array();
  for (const auto& r : ranked) {
    const auto& m = r.m;
    const std::string& abs = r.abs_path;
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

json Server::onHover(const json& params, std::atomic_bool* cancelled, std::atomic<pid_t>* child_pid) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return nullResult();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return nullResult();
  if (isInLineCommentAt(it->second.text, line0, ch0)) return nullResult();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (isStopWord(sym)) return nullResult();
  std::string current_abs = makeResultPathAbsolute(fileUriToPath(uri));
  int current_line1 = line0 + 1;

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 20, cancelled, child_pid);
  } else {
    matches = grepFixedString(rootDir(), sym, 20, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"), cancelled, child_pid);
  }
  if (matches.empty()) return nullResult();

  auto ranked = rankAndFilterMatches(matches, sym, current_abs, current_line1, /*prefer_abs_path=*/current_abs,
                                     [this](const std::string& p) { return makeResultPathAbsolute(p); });
  if (ranked.empty()) return nullResult();
  const auto& best = ranked.front();
  const auto& m = best.m;
  const std::string& abs = best.abs_path;
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

json Server::onDefinition(const json& params, std::atomic_bool* cancelled, std::atomic<pid_t>* child_pid) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return nullResult();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return nullResult();
  if (isInLineCommentAt(it->second.text, line0, ch0)) return nullResult();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (isStopWord(sym)) return nullResult();
  std::string current_abs = makeResultPathAbsolute(fileUriToPath(uri));
  int current_line1 = line0 + 1;

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 20, cancelled, child_pid);
  } else {
    matches = grepFixedString(rootDir(), sym, 20, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"), cancelled, child_pid);
  }
  if (matches.empty()) return nullResult();

  auto ranked = rankAndFilterMatches(matches, sym, current_abs, current_line1, /*prefer_abs_path=*/current_abs,
                                     [this](const std::string& p) { return makeResultPathAbsolute(p); });
  if (ranked.empty()) return nullResult();

  // If there's exactly one "strong" definition-like hit, return only it.
  // (VSCode will then jump directly instead of showing a chooser.)
  int strong = 0;
  std::size_t strong_idx = 0;
  for (std::size_t i = 0; i < ranked.size(); ++i) {
    if (ranked[i].score >= 60) {
      strong++;
      strong_idx = i;
      if (strong > 1) break;
    }
  }

  json locs = json::array();
  if (strong == 1) {
    const auto& r = ranked[strong_idx];
    const auto& m = r.m;
    const std::string& abs = r.abs_path;
    json loc;
    loc["uri"] = pathToFileUri(abs);
    loc["range"] = json{
        {"start", json{{"line", m.line - 1}, {"character", m.column}}},
        {"end", json{{"line", m.line - 1}, {"character", m.column + static_cast<int>(sym.size())}}},
    };
    locs.push_back(std::move(loc));
    return locs;
  }

  for (const auto& r : ranked) {
    const auto& m = r.m;
    const std::string& abs = r.abs_path;
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

json Server::onReferences(const json& params, std::atomic_bool* cancelled, std::atomic<pid_t>* child_pid) {
  auto td = params.value("textDocument", json::object());
  std::string uri = getStringOr(td, "uri");
  if (uri.empty()) return json::array();

  auto pos = params.value("position", json::object());
  int line0 = getIntOr(pos, "line", 0);
  int ch0 = getIntOr(pos, "character", 0);

  auto it = docs_by_uri_.find(uri);
  if (it == docs_by_uri_.end()) return json::array();
  if (isInLineCommentAt(it->second.text, line0, ch0)) return json::array();
  std::string sym = wordAt(it->second.text, line0, ch0);
  if (isStopWord(sym)) return json::array();
  std::string current_abs = makeResultPathAbsolute(fileUriToPath(uri));
  int current_line1 = line0 + 1;

  std::vector<GrepMatch> matches;
  if (!serve_files_.empty()) {
    matches = grepFixedStringInFiles(serve_files_, sym, 50, cancelled, child_pid);
  } else {
    matches = grepFixedString(rootDir(), sym, 50, std::string("c,cc,cpp,cxx,h,hh,hpp,hxx"), cancelled, child_pid);
  }

  auto ranked = rankAndFilterMatches(matches, sym, current_abs, current_line1, /*prefer_abs_path=*/current_abs,
                                     [this](const std::string& p) { return makeResultPathAbsolute(p); });
  json locs = json::array();
  for (const auto& r : ranked) {
    const auto& m = r.m;
    const std::string& abs = r.abs_path;
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
  std::lock_guard<std::mutex> lg(send_mu_);
  transport_.writeMessage(resp.dump());
}

void Server::replyError(const json& id, int code, const std::string& message) {
  json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  resp["error"] = json{{"code", code}, {"message", message}};
  std::lock_guard<std::mutex> lg(send_mu_);
  transport_.writeMessage(resp.dump());
}

void Server::sendNotification(const std::string& method, const json& params) {
  json msg;
  msg["jsonrpc"] = "2.0";
  msg["method"] = method;
  msg["params"] = params;
  std::lock_guard<std::mutex> lg(send_mu_);
  transport_.writeMessage(msg.dump());
}

}  // namespace slclangd::lsp


