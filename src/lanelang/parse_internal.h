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

#ifndef LANELANG_PARSE_INTERNAL_H
#define LANELANG_PARSE_INTERNAL_H

#include "ast.h"
#include "diagnostic.h"
#include "lexer.h"

#include <stddef.h>

// ARENA_CHUNK_BYTES is how many bytes an arena chunk holds when the request that
// triggered it is smaller than this. A larger request gets a chunk of its own size, so no
// allocation can fail for want of chunk room.
#define ARENA_CHUNK_BYTES 8192

// ARENA_ALIGNMENT is the multiple to which each allocation size is rounded. The first
// allocation starts at the chunk's data field.
#define ARENA_ALIGNMENT 16

// MAX_PARAMETERS is how many parameters one declaration may have, gathered on the stack
// and copied to the arena once counted.
#define MAX_PARAMETERS 8

// MAX_BINDINGS is how many `name := expr` bindings one function body may have, gathered
// on the stack in the same way.
#define MAX_BINDINGS 64

// MAX_FUNCTIONS is how many functions one source file may declare, gathered on the stack
// in the same way.
#define MAX_FUNCTIONS 32

// LANELANG_MAX_EXPR_DEPTH is how deep expression nesting may go before the parser reports
// a structural failure.
#define LANELANG_MAX_EXPR_DEPTH 256

// chunk_t is one block of arena storage. Nodes are handed out from `data` and never freed
// individually.
typedef struct chunk {
    // next is the chunk allocated before this one, or NULL at the end of the list.
    struct chunk *next;

    // used is how many bytes of `data` have been handed out.
    size_t used;

    // capacity is how many bytes of `data` exist.
    size_t capacity;

    // data is the storage itself, over-allocated past this one declared byte.
    char data[1];
} chunk_t;

// arena_t is a bump arena, which is a list of chunks with the newest first.
typedef struct {
    // head is the newest chunk, which is the one being allocated from, or NULL before the
    // first request.
    chunk_t *head;
} arena_t;

// parser_t is the parser's whole state, small enough to copy wherever the parser needs to
// look ahead by two tokens.
typedef struct {
    // lexer is the token source.
    lexer_t lexer;

    // token is the token about to be consumed, which is the parser's single token of
    // lookahead.
    token_t token;

    // diagnostics is where failures are reported.
    diagnostic_t *diagnostics;

    // arena is where nodes are allocated from.
    arena_t *arena;

    // failed records a parse or allocation failure. The parser discards the tree when
    // this flag is set.
    int failed;

    // depth counts active expression parses and is bounded by LANELANG_MAX_EXPR_DEPTH.
    int depth;
} parser_t;

// arena_alloc hands out `size` bytes of arena storage, aligned and zeroed.
//
// Takes arena (arena_t) which is the allocator the tree is built in.
// Takes size (size_t) which is how many bytes are wanted.
// Returns the storage, or NULL if allocation fails.
void *arena_alloc(arena_t *arena, const size_t size);

// parse_copy_to_arena copies `count` items into the arena and returns the copy.
//
// An empty array returns NULL without reading `items`. For a positive count, NULL means
// allocation failed.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes items (void) which is the array being copied.
// Takes item_size (size_t) which is one item's size in bytes.
// Takes count (int) which is how many items there are.
// Returns the copy, NULL for a count of zero, or NULL after marking the parse failed.
void *parse_copy_to_arena(parser_t *parser, const void *items, const size_t item_size,
                          const int count);

// parse_advance consumes the current token and reads the next.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
void parse_advance(parser_t *parser);

// parse_accept consumes the current token when it is of `kind`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes kind (token_kind_t) which is the kind being looked for.
// Returns 1 when the token was consumed, 0 otherwise.
int parse_accept(parser_t *parser, const token_kind_t kind);

// parse_expect consumes a token of `kind`, reporting when there is none.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes kind (token_kind_t) which is the kind required.
// Returns 1 on a match or 0 if the token differs.
int parse_expect(parser_t *parser, const token_kind_t kind);

// parse_accept_keyword consumes the current token when it spells `keyword`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes keyword (char) which is the word being looked for.
// Returns 1 when the token was consumed, 0 otherwise.
int parse_accept_keyword(parser_t *parser, const char *keyword);

// parse_expect_keyword consumes `keyword`, reporting when it is not there.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes keyword (char) which is the word required.
// Returns 1 on a match or 0 if the token differs.
int parse_expect_keyword(parser_t *parser, const char *keyword);

// parse_expr parses one expression, guarding the recursion depth.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Returns the expression or NULL on failure. The caller must also check the parser's
// failure flag before using the completed tree.
expr_t *parse_expr(parser_t *parser);

// parse_bless reads an `@bless(...)` waiver at the cursor, consuming nothing when there
// is none.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Returns 0 for an absent waiver, BLESS_PROMISE for a bare promise, or a checksum biased
// by one. The caller checks the parser's failure flag before using the result.
long long parse_bless(parser_t *parser);

// qualifiers_t holds an optional grounding qualifier followed by an optional lane. When
// both are present, `ungrounded replay Column[Float]` follows this order and `replay
// ungrounded Column[Float]` is refused.
typedef struct {
    // lane is the demand written, or LANE_NONE.
    lane_t lane;

    // ungrounded is whether the grounding qualifier was written.
    int ungrounded;

    // lane_span is the lane word's source text, for blame. It covers that one word rather
    // than the whole prefix, so a diagnostic underlines the word it is talking about.
    span_t lane_span;

    // ungrounded_span is the grounding word's source text, for blame.
    span_t ungrounded_span;
} qualifiers_t;

// parse_bindings reads the `name := expr` bindings at the head of a body into `bindings`,
// which holds MAX_BINDINGS entries, stopping at `return`.
//
// It is shared by a function's body and a step function's braced body, so a binding means
// the same in both. A name already bound in the body, or already one of its parameters,
// is refused.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes bindings (bind_t) which receives the bindings in source order.
// Takes parameters (parameter_t) which holds the body's parameters.
// Takes parameter_count (int) which is how many parameters the body has.
// Returns how many were read, or -1 on failure.
int parse_bindings(parser_t *parser, bind_t *bindings, const parameter_t *parameters,
                   int parameter_count);

#endif  // LANELANG_PARSE_INTERNAL_H
