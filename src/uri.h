#pragma once

#include <string>

namespace slclangd::lsp {

// Minimal file:// URI helpers for Linux paths.
std::string pathToFileUri(const std::string& path);
std::string fileUriToPath(const std::string& uri);

}  // namespace slclangd::lsp


