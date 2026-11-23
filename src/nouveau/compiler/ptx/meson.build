# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: MIT
#
# CloudBSD fork: libnvptx, the PTX front end of native CUDA (Track #2426).

libnvptx = static_library(
  'nvptx',
  files(
    'ptx_lex.c',
    'ptx_parse.c',
    'ptx_print.c',
  ),
  dependencies : [idep_mesautil],
  include_directories : include_directories('.'),
)

nvptx_parse_test = executable(
  'nvptx_parse_test',
  files('tests/ptx_parse_test.c'),
  link_with : libnvptx,
  dependencies : [idep_mesautil],
  include_directories : include_directories('.'),
)

test(
  'nvptx_parse',
  nvptx_parse_test,
  suite : ['nvptx'],
  workdir : meson.current_source_dir() / 'tests' / 'data',
)
