#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

namespace slclangd::lsp {

// Minimal LSP/JSON-RPC transport: reads/writes "Content-Length:" framed messages over stdio.
class Transport final {
 public:
  Transport(std::istream& in, std::ostream& out, std::ostream& log);

  // Returns nullopt on clean EOF.
  std::optional<std::string> readMessage();

  void writeMessage(const std::string& json);

  void logLine(const std::string& s);

 private:
  std::istream& in_;
  std::ostream& out_;
  std::ostream& log_;
};

}  // namespace slclangd::lsp


