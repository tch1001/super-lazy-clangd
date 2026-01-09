#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "lsp_server.h"
#include "lsp_transport.h"

namespace {

static void printHelp() {
  std::cerr << "super-lazy-clangd (tiny LSP, grep-backed)\n\n"
               "Usage:\n"
               "  super-lazy-clangd [--files <file1> <file2> ...]\n\n"
               "Options:\n"
               "  --files    Restrict search to this explicit list of files.\n"
               "            Tip: use `--` if you have a filename starting with '-'.\n"
               "  --log-file <path>\n"
               "            Write server logs/trace to this file (useful for VSCode debugging).\n"
               "            (If unset, also checks env var CLANGD_TRACE as a fallback.)\n"
               "  --version  Print version and exit.\n"
               "  -h,--help  Show help.\n";
}

static std::string normalizePath(const std::string& p) {
  try {
    return std::filesystem::absolute(std::filesystem::path(p)).lexically_normal().string();
  } catch (...) {
    return p;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> serve_files;
  std::string log_file;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printHelp();
      return 0;
    }
    if (arg == "--version") {
      std::cout << "super-lazy-clangd 0.1.0\n";
      return 0;
    }
    if (arg == "--log-file") {
      if (i + 1 < argc) {
        log_file = argv[++i];
      }
      continue;
    }
    if (arg == "--files") {
      ++i;
      for (; i < argc; ++i) {
        std::string f = argv[i];
        if (f == "--") continue;
        if (f.rfind("--", 0) == 0) {
          --i;
          break;
        }
        serve_files.push_back(normalizePath(f));
      }
      continue;
    }
  }

  std::ofstream log_ofs;
  std::ostream* log = &std::cerr;
  if (log_file.empty()) {
    if (const char* p = std::getenv("CLANGD_TRACE")) {
      // VSCode clangd extension sets CLANGD_TRACE (clangd.trace) to a filepath.
      // We reuse it as a simple log sink to make debugging easy.
      if (*p) log_file = p;
    }
  }
  if (!log_file.empty()) {
    log_ofs.open(log_file, std::ios::out | std::ios::app);
    if (log_ofs.is_open()) log = &log_ofs;
  }

  slclangd::lsp::Transport transport(std::cin, std::cout, *log);
  slclangd::lsp::Server server(transport, std::move(serve_files));
  return server.run();
}


