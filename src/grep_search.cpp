#include "grep_search.h"

#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace slclangd {
namespace {

static bool splitFirstTwoColons(const std::string& s, std::string& a, std::string& b, std::string& rest) {
  auto p1 = s.find(':');
  if (p1 == std::string::npos) return false;
  auto p2 = s.find(':', p1 + 1);
  if (p2 == std::string::npos) return false;
  a = s.substr(0, p1);
  b = s.substr(p1 + 1, p2 - (p1 + 1));
  rest = s.substr(p2 + 1);
  return true;
}

static int findColumn0(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;

  // Filter comment-only lines: first two non-space characters are "//".
  std::size_t i = 0;
  while (i < haystack.size() && std::isspace(static_cast<unsigned char>(haystack[i]))) ++i;
  if (i + 1 < haystack.size() && haystack[i] == '/' && haystack[i + 1] == '/') return -1;

  // Find the first occurrence of needle that is NOT inside double quotes.
  // (Very lightweight heuristic: handles \" escaping, doesn't parse C++ fully.)
  auto is_escaped_quote = [&](std::size_t pos) -> bool {
    // Count backslashes directly preceding pos.
    std::size_t bs = 0;
    while (pos > 0 && haystack[pos - 1] == '\\') {
      ++bs;
      --pos;
    }
    return (bs % 2) == 1;
  };

  std::size_t search_from = 0;
  while (true) {
    std::size_t pos = haystack.find(needle, search_from);
    if (pos == std::string::npos) return -1;

    bool in_string = false;
    for (std::size_t j = 0; j < pos; ++j) {
      if (haystack[j] == '"' && !is_escaped_quote(j)) in_string = !in_string;
    }
    if (!in_string) return static_cast<int>(pos);

    search_from = pos + 1;
  }
}

static void closeIfValid(int fd) {
  if (fd >= 0) close(fd);
}

static std::vector<GrepMatch> runGrep(const std::vector<std::string>& args_str,
                                      const std::string& needle,
                                      int max_results) {
  std::vector<GrepMatch> out;
  if (needle.empty() || max_results <= 0) return out;

  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) {
    return out;
  }

  pid_t pid = fork();
  if (pid == -1) {
    closeIfValid(pipefd[0]);
    closeIfValid(pipefd[1]);
    return out;
  }

  if (pid == 0) {
    // child
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)dup2(pipefd[1], STDERR_FILENO);  // keep things simple
    closeIfValid(pipefd[0]);
    closeIfValid(pipefd[1]);

    std::vector<char*> argv;
    argv.reserve(args_str.size() + 1);
    for (auto& s : const_cast<std::vector<std::string>&>(args_str)) argv.push_back(s.data());
    argv.push_back(nullptr);

    execvp("grep", argv.data());
    _exit(127);
  }

  // parent
  closeIfValid(pipefd[1]);
  FILE* f = fdopen(pipefd[0], "r");
  if (!f) {
    closeIfValid(pipefd[0]);
    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, nullptr, 0);
    return out;
  }

  char* lineptr = nullptr;
  size_t n = 0;
  int collected = 0;
  while (true) {
    errno = 0;
    ssize_t r = getline(&lineptr, &n, f);
    if (r <= 0) break;
    std::string line(lineptr, static_cast<std::size_t>(r));
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    std::string path, lno, text;
    if (!splitFirstTwoColons(line, path, lno, text)) {
      continue;
    }
    int line_no = 0;
    try {
      line_no = std::stoi(lno);
    } catch (...) {
      continue;
    }

    GrepMatch m;
    m.path = path;
    m.line = line_no;
    m.text = text;
    m.column = findColumn0(text, needle);
    if (m.column < 0) {
      continue;  // filtered out (comment-only line or match only in quotes)
    }
    out.push_back(std::move(m));
    if (++collected >= max_results) {
      (void)kill(pid, SIGTERM);
      break;
    }
  }
  if (lineptr) free(lineptr);
  fclose(f);

  int status = 0;
  (void)waitpid(pid, &status, 0);
  return out;
}

}  // namespace

std::vector<GrepMatch> grepFixedString(const std::string& root_dir,
                                       const std::string& needle,
                                       int max_results,
                                       std::optional<std::string> only_extensions) {
  std::vector<std::string> args_str;
  args_str.push_back("grep");
  args_str.push_back("-RIn");          // recursive, line numbers
  args_str.push_back("--binary-files=without-match");
  args_str.push_back("--color=never");
  args_str.push_back("--exclude-dir=build");
  args_str.push_back("--exclude-dir=.git");

  // Best-effort extension filter (if caller wants it):
  // GNU grep's --include works with glob patterns; we accept a comma-separated list like "cpp,hpp,h".
  if (only_extensions && !only_extensions->empty()) {
    std::istringstream iss(*only_extensions);
    std::string ext;
    while (std::getline(iss, ext, ',')) {
      if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
      if (ext.empty()) continue;
      args_str.push_back(std::string("--include=*.") + ext);
    }
  }

  args_str.push_back("-F");
  args_str.push_back("--");
  args_str.push_back(needle);
  args_str.push_back(root_dir);

  return runGrep(args_str, needle, max_results);
}

std::vector<GrepMatch> grepFixedStringInFiles(const std::vector<std::string>& files,
                                              const std::string& needle,
                                              int max_results) {
  if (files.empty()) return {};

  std::vector<std::string> args_str;
  args_str.push_back("grep");
  args_str.push_back("-nH");  // line numbers + always print filename
  args_str.push_back("--binary-files=without-match");
  args_str.push_back("--color=never");
  args_str.push_back("-F");
  args_str.push_back("--");
  args_str.push_back(needle);
  for (const auto& f : files) args_str.push_back(f);

  return runGrep(args_str, needle, max_results);
}

}  // namespace slclangd


