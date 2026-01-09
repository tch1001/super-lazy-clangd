#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <sys/types.h>
#include <vector>

#include "lsp_transport.h"

// Vendored single-header nlohmann::json
#include "json.hpp"

namespace slclangd::lsp {

class Server final {
 public:
  explicit Server(Transport& transport, std::vector<std::string> serve_files = {});

  // Runs the main loop until exit.
  int run();

 private:
  void handleMessage(const std::string& body);

  void handleRequest(const std::string& method, const nlohmann::json& id, const nlohmann::json& params);
  void handleNotification(const std::string& method, const nlohmann::json& params);

  nlohmann::json onInitialize(const nlohmann::json& params);
  void onInitialized(const nlohmann::json& params);
  nlohmann::json onShutdown();
  void onExit();

  void onDidOpen(const nlohmann::json& params);
  void onDidChange(const nlohmann::json& params);
  void onDidClose(const nlohmann::json& params);

  nlohmann::json onWorkspaceSymbol(const nlohmann::json& params,
                                   std::atomic_bool* cancelled,
                                   std::atomic<pid_t>* child_pid);
  nlohmann::json onHover(const nlohmann::json& params,
                         std::atomic_bool* cancelled,
                         std::atomic<pid_t>* child_pid);
  nlohmann::json onDefinition(const nlohmann::json& params,
                              std::atomic_bool* cancelled,
                              std::atomic<pid_t>* child_pid);
  nlohmann::json onReferences(const nlohmann::json& params,
                              std::atomic_bool* cancelled,
                              std::atomic<pid_t>* child_pid);

  void replyResult(const nlohmann::json& id, const nlohmann::json& result);
  void replyError(const nlohmann::json& id, int code, const std::string& message);
  void sendNotification(const std::string& method, const nlohmann::json& params);

  std::string rootDir() const;
  std::string makeResultPathAbsolute(const std::string& p) const;

  Transport& transport_;
  bool shutdown_received_ = false;
  bool exit_requested_ = false;
  bool trace_ = false;
  bool clangd_file_status_ = false;

  struct InFlight {
    std::atomic_bool cancelled{false};
    std::atomic<pid_t> grep_pid{-1};
  };
  std::mutex inflight_mu_;
  std::unordered_map<std::string, std::shared_ptr<InFlight>> inflight_;
  std::mutex send_mu_;

  std::string root_uri_;
  std::string root_path_;
  std::vector<std::string> serve_files_;

  struct Doc {
    std::string text;
  };
  std::unordered_map<std::string, Doc> docs_by_uri_;
};

}  // namespace slclangd::lsp


