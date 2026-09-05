// Copyright 2026 PolitePixels Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LANELANG_PARSE_H
#define LANELANG_PARSE_H

#include "ast.h"
#include "diagnostic.h"

// parse_program parses `source`, whose name is `file`, reporting through `diagnostics`.
// Returns NULL on any syntax error, having reported at least one. The returned program
// borrows `source`, which must outlive it; release it with ast_free.
//
// Takes file (char) which is the file being read.
// Takes source (char) which is the source text, borrowed.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
program_t *parse_program(const char *file, const char *source, diagnostic_t *diagnostics);

#endif  // LANELANG_PARSE_H
