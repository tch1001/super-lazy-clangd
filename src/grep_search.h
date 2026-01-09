#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace slclangd {

struct GrepMatch {
  std::string path;   // absolute or relative path as reported by grep
  int line = 0;       // 1-based
  int column = 0;     // 0-based (best-effort)
  std::string text;   // the full line text (best-effort)
};

// Runs GNU grep recursively and returns matches. Uses fixed-string search (-F).
std::vector<GrepMatch> grepFixedString(const std::string& root_dir,
                                       const std::string& needle,
                                       int max_results,
                                       std::optional<std::string> only_extensions = std::nullopt,
                                       std::atomic_bool* cancelled = nullptr,
                                       std::atomic<pid_t>* child_pid = nullptr);

// Runs GNU grep over an explicit list of file paths. Uses fixed-string search (-F).
std::vector<GrepMatch> grepFixedStringInFiles(const std::vector<std::string>& files,
                                              const std::string& needle,
                                              int max_results,
                                              std::atomic_bool* cancelled = nullptr,
                                              std::atomic<pid_t>* child_pid = nullptr);

}  // namespace slclangd


