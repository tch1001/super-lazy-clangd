#include "uri.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace slclangd::lsp {
namespace {

static inline bool isUnreserved(unsigned char c) {
  // RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
  return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

static std::string pctEncode(const std::string& s) {
  std::ostringstream oss;
  oss << std::hex;
  for (unsigned char c : s) {
    if (isUnreserved(c)) {
      oss << static_cast<char>(c);
    } else {
      const char* hex = "0123456789ABCDEF";
      oss << '%' << hex[(c >> 4) & 0xF] << hex[c & 0xF];
    }
  }
  return oss.str();
}

static int fromHex(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static std::string pctDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = fromHex(static_cast<unsigned char>(s[i + 1]));
      int lo = fromHex(static_cast<unsigned char>(s[i + 2]));
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

}  // namespace

std::string pathToFileUri(const std::string& path) {
  // Assume absolute POSIX path.
  return "file://" + pctEncode(path);
}

std::string fileUriToPath(const std::string& uri) {
  constexpr const char* kPrefix = "file://";
  if (uri.rfind(kPrefix, 0) != 0) return uri;
  return pctDecode(uri.substr(7));
}

}  // namespace slclangd::lsp


