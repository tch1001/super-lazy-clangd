#include "lsp_transport.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

namespace slclangd::lsp {
namespace {

static inline std::string trim(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(),
          s.end());
  return s;
}

}  // namespace

Transport::Transport(std::istream& in, std::ostream& out, std::ostream& log) : in_(in), out_(out), log_(log) {}

std::optional<std::string> Transport::readMessage() {
  std::string line;
  std::size_t content_length = 0;
  bool saw_header = false;

  while (true) {
    if (!std::getline(in_, line)) {
      // EOF
      return std::nullopt;
    }

    // getline strips '\n' but keeps a trailing '\r' if present.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.empty()) {
      break;  // end of headers
    }

    saw_header = true;
    constexpr const char* kCL = "Content-Length:";
    if (line.rfind(kCL, 0) == 0) {
      auto v = trim(line.substr(std::char_traits<char>::length(kCL)));
      try {
        content_length = static_cast<std::size_t>(std::stoul(v));
      } catch (...) {
        log_ << "Invalid Content-Length value: " << v << "\n";
        content_length = 0;
      }
    }
  }

  if (!saw_header && in_.eof()) {
    return std::nullopt;
  }
  if (content_length == 0) {
    // Some clients may send empty notifications; treat as no-op.
    return std::string{};
  }

  std::string body(content_length, '\0');
  in_.read(body.data(), static_cast<std::streamsize>(content_length));
  if (static_cast<std::size_t>(in_.gcount()) != content_length) {
    log_ << "Short read: expected " << content_length << " bytes, got " << in_.gcount() << "\n";
    return std::nullopt;
  }
  return body;
}

void Transport::writeMessage(const std::string& json) {
  out_ << "Content-Length: " << json.size() << "\r\n\r\n";
  out_ << json;
  out_.flush();
}

void Transport::logLine(const std::string& s) { log_ << s << "\n"; }

}  // namespace slclangd::lsp


