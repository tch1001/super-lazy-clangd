project(
  'super-lazy-clangd',
  'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=false',
  ],
)

src = files(
  'src/main.cpp',
  'src/lsp_transport.cpp',
  'src/lsp_server.cpp',
  'src/grep_search.cpp',
  'src/uri.cpp',
)

executable(
  'super-lazy-clangd',
  src,
  include_directories: include_directories('third_party'),
  install: true,
)


